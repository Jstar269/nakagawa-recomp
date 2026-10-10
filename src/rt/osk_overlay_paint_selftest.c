/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_overlay_paint_selftest.c - the in-window keyboard drawn over a synthetic frame
 * (src/rt/osk_overlay_paint.c), with the SDL software renderer and no window. Checks exact
 * pixels: the highlighted key, an enabled key, a key the input type disables, the text field,
 * and that a closed keyboard leaves the frame untouched. A surface or renderer that SDL refuses
 * after the probe passed must fail closed: the open request is dropped, the host is taken away,
 * the frame is left alone and one stderr line names the SDL error. With an output path it also
 * writes the frame as a BMP, the visual evidence for the keyboard over a synthetic background.
 * The frame is generated here; no game data is read.
 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* fileno, dup, dup2 for the stderr capture below */
#endif

#include "osk_overlay_paint.h"

#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_surface.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define FD_OF    _fileno
#define FD_DUP   _dup
#define FD_DUP2  _dup2
#define FD_CLOSE _close
#else
#include <unistd.h>
#define FD_OF    fileno
#define FD_DUP   dup
#define FD_DUP2  dup2
#define FD_CLOSE close
#endif

#define W 480
#define H 272

/* The painter's test seam (osk_overlay_paint.c): the next creations fail as SDL would refuse. */
void osk_overlay_paint_test_fail_next(int surfaces, int renderers);

static int s_checks;
static int s_failures;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        s_checks++;                                                       \
        if (!(cond)) {                                                    \
            s_failures++;                                                 \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                                 \
    } while (0)

static uint32_t s_frame[W * H];
static uint32_t s_before[W * H];

/* A synthetic background: a colour ramp, so a dimmed or overwritten pixel is visible. */
static void make_background(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            s_frame[y * W + x] = ((uint32_t)(x * 255 / (W - 1)) << 16) |
                                 ((uint32_t)(y * 255 / (H - 1)) << 8) | 0x60u;
}

static uint32_t px(int x, int y) { return s_frame[y * W + x]; }

static uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return (r << 16) | (g << 8) | b; }

/* Key rectangles, as osk_overlay_paint.c lays them out: column unit 44, row pitch 28. */
static int key_x(int start) { return 20 + start * 44 + 2; }
static int key_y(int row) { return 56 + row * 28; }

static void write_bmp(const char *path) {
    SDL_Surface *surface = SDL_CreateSurfaceFrom(W, H, SDL_PIXELFORMAT_XRGB8888, s_frame, W * 4);
    if (!surface) {
        fprintf(stderr, "osk_overlay_paint_selftest: SDL_CreateSurfaceFrom failed: %s\n",
                SDL_GetError());
        return;
    }
    if (!SDL_SaveBMP(surface, path))
        fprintf(stderr, "osk_overlay_paint_selftest: SDL_SaveBMP failed: %s\n", SDL_GetError());
    SDL_DestroySurface(surface);
}

static void test_closed_keyboard_draws_nothing(void) {
    OskOverlay o;
    osk_overlay_reset(&o);
    memcpy(s_before, s_frame, sizeof s_frame);
    osk_overlay_paint(&o, s_frame, W, H);
    CHECK(memcmp(s_before, s_frame, sizeof s_frame) == 0,
          "a closed keyboard leaves the frame byte-for-byte unchanged");
}

static void test_open_keyboard(const char *bmp_path) {
    OskOverlay o;
    static const uint16_t desc[] = { 'E', 'n', 't', 'e', 'r', ' ', 'n', 'a', 'm', 'e', '.', 0 };
    static const uint16_t initial[] = { 'A', 'C', 'E', 0 };
    osk_overlay_reset(&o);
    osk_overlay_open(&o, desc, initial, 16, 0);
    make_background();
    osk_overlay_paint(&o, s_frame, W, H);

    /* The highlighted key is A: row 1, column 0. Its fill is the highlight colour, away from the
     * glyph (which starts 16 pixels in). */
    CHECK(px(key_x(0) + 4, key_y(1) + 4) == rgb(70, 120, 200),
          "the highlighted key (A) is filled with the highlight colour");
    /* B is an enabled key next to it: the normal key colour. */
    CHECK(px(key_x(1) + 4, key_y(1) + 4) == rgb(44, 56, 80),
          "an enabled key (B) is filled with the normal key colour");
    /* The text field: its interior is the field colour, drawn over the background. */
    CHECK(px(14, 26) == rgb(30, 36, 50), "the text field is filled");
    /* The panel is drawn over the background, so a corner that was 255,255,96 no longer is. */
    CHECK(px(W - 1, H - 1) != rgb(255, 255, 0x60u), "the panel dims the synthetic background");

    if (bmp_path) write_bmp(bmp_path);
}

static void test_input_type_dims_keys(void) {
    OskOverlay o;
    static const uint16_t initial[] = { '1', 0 };
    osk_overlay_reset(&o);
    osk_overlay_open(&o, NULL, initial, 16, OSK_INPUT_LATIN_DIGIT);
    make_background();
    osk_overlay_paint(&o, s_frame, W, H);
    /* Digit-only: '1' is highlighted, the letter B is disabled and dimmed. */
    CHECK(px(key_x(0) + 4, key_y(0) + 4) == rgb(70, 120, 200),
          "digit-only: the highlighted key is the digit 1");
    CHECK(px(key_x(1) + 4, key_y(1) + 4) == rgb(24, 28, 38),
          "digit-only: the letter B is disabled and dimmed");
}

static void test_outline_and_caret(void) {
    OskOverlay o;
    static const uint16_t initial[] = { 'x', 0 };
    osk_overlay_reset(&o);
    osk_overlay_open(&o, NULL, initial, 16, 0);
    make_background();
    osk_overlay_paint(&o, s_frame, W, H);
    /* The field border is drawn on its top edge. */
    CHECK(px(200, 24) == rgb(90, 120, 180), "the text field has its outline");
}

/* Stderr goes to a temporary file while the painter runs, so the selftest can count the lines
 * it writes. Returns 0 when the capture could not start; stderr is then left as it was. */
static int capture_begin(FILE **capture, int *saved) {
    *capture = tmpfile();
    if (!*capture) return 0;
    fflush(stderr);
    *saved = FD_DUP(FD_OF(stderr));
    if (*saved < 0 || FD_DUP2(FD_OF(*capture), FD_OF(stderr)) < 0) {
        if (*saved >= 0) FD_CLOSE(*saved);
        fclose(*capture);
        *capture = NULL;
        return 0;
    }
    return 1;
}

/* Restores stderr and copies what was captured into out (NUL-terminated, truncated to cap). */
static size_t capture_end(FILE *capture, int saved, char *out, size_t cap) {
    size_t n;
    fflush(stderr);
    FD_DUP2(saved, FD_OF(stderr));
    FD_CLOSE(saved);
    rewind(capture);
    n = fread(out, 1, cap - 1, capture);
    out[n] = 0;
    fclose(capture);
    return n;
}

/* One creation SDL refuses after the probe passed: the keyboard must fail closed. The open
 * request is dropped and the host goes away (the next poll answers through the native input
 * path), the painter reports itself unavailable, the frame is left as it was, and exactly one
 * stderr line names the SDL error (expect is the text the refusal reports). */
static void run_creation_failure(int surfaces, int renderers, const char *expect) {
    wchar_t out[16];
    char captured[512] = "";
    FILE *capture = NULL;
    int saved = -1;
    int capturing;
    size_t n = 0, lines = 0;

    sr_osk_overlay_abandon();
    sr_osk_overlay_set_host(1);
    CHECK(sr_osk_overlay_poll(L"Name", L"x", out, 16, 0) == SR_OSK_TEXT_PENDING,
          "the session opens a request while the overlay host is up");
    CHECK(sr_osk_overlay_active(), "the request is open before the painter fails");
    make_background();
    memcpy(s_before, s_frame, sizeof s_frame);

    osk_overlay_paint_test_fail_next(surfaces, renderers);
    capturing = capture_begin(&capture, &saved);
    CHECK(capturing, "stderr can be captured for the failure line");
    osk_overlay_paint(sr_osk_overlay_view(), s_frame, W, H);
    if (capturing) n = capture_end(capture, saved, captured, sizeof captured);
    osk_overlay_paint_test_fail_next(0, 0);

    for (size_t i = 0; i < n; i++)
        if (captured[i] == '\n') lines++;
    CHECK(!sr_osk_overlay_active(),
          "a refused creation drops the open request (no invisible keyboard stays open)");
    CHECK(sr_osk_overlay_host() == 0, "a refused creation takes the overlay host away");
    CHECK(!osk_overlay_paint_available(), "a refused creation makes the painter unavailable");
    CHECK(memcmp(s_before, s_frame, sizeof s_frame) == 0,
          "a refused creation leaves the frame byte-for-byte unchanged");
    CHECK(lines == 1, "a refused creation writes exactly one stderr line");
    CHECK(strstr(captured, expect) != NULL, "the stderr line names the SDL error");

    sr_osk_overlay_abandon();
    sr_osk_overlay_set_host(0);
}

static void test_creation_failure_fails_closed(void) {
    run_creation_failure(1, 0, "test seam: surface creation refused");
    run_creation_failure(0, 1, "test seam: renderer creation refused");
}

int main(int argc, char **argv) {
    const char *bmp_path = argc > 1 ? argv[1] : NULL;
    if (!osk_overlay_paint_available()) {
        printf("osk_overlay_paint_selftest: SKIP (SDL cannot draw a software renderer here)\n");
        return 0;
    }
    test_closed_keyboard_draws_nothing();
    test_open_keyboard(bmp_path);
    test_input_type_dims_keys();
    test_outline_and_caret();
    test_creation_failure_fails_closed();
    printf("osk_overlay_paint_selftest: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
