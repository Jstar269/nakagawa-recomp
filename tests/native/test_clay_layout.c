// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include <stdio.h>
#include <stdlib.h>

#define CLAY_IMPLEMENTATION
#include "../../third_party/clay/clay.h"

static int clay_error_count;

static void record_clay_error(Clay_ErrorData error)
{
    clay_error_count++;
    fprintf(stderr, "Clay error %d\n", (int)error.errorType);
}

static Clay_Dimensions measure_text(Clay_StringSlice text,
                                    Clay_TextElementConfig *config,
                                    void *user_data)
{
    (void)text;
    (void)config;
    (void)user_data;
    return (Clay_Dimensions){ 0.0f, 0.0f };
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
    CLAY({
        .id = CLAY_ID("root"),
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
        CLAY({
            .id = CLAY_ID("fixed-a"),
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(20.0f),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
        CLAY({
            .id = CLAY_ID("fixed-b"),
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_FIXED(30.0f),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
        CLAY({
            .id = CLAY_ID("grow"),
            .layout = {
                .sizing = {
                    .width = CLAY_SIZING_GROW(0),
                    .height = CLAY_SIZING_FIXED(20.0f),
                },
            },
        }) {}
    }
    (void)Clay_EndLayout();

    if (clay_error_count != 0 ||
        !expect_rectangle("root", CLAY_ID("root"), 0.0f, 0.0f, 120.0f, 80.0f) ||
        !expect_rectangle("fixed-a", CLAY_ID("fixed-a"), 10.0f, 10.0f, 20.0f, 20.0f) ||
        !expect_rectangle("fixed-b", CLAY_ID("fixed-b"), 35.0f, 10.0f, 30.0f, 20.0f) ||
        !expect_rectangle("grow", CLAY_ID("grow"), 70.0f, 10.0f, 40.0f, 20.0f)) {
        free(memory);
        return EXIT_FAILURE;
    }

    free(memory);
    puts("Clay layout selftest passed: root, padding, child gap, fixed children, and grow child");
    return EXIT_SUCCESS;
}
