/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <psputility.h>
#include <psputility_savedata.h>
#include <string.h>

#include "../common/tone.h"

PSP_MODULE_INFO("NAKAGAWA_SHOWCASE_BREAKOUT", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define FRAME_WIDTH 512
#define BRICK_ROWS 5
#define BRICK_COLS 9
#define SAVE_GAME_NAME "NKGSHOW"
#define SAVE_NAME "BREAKOUT"
#define MAX_RECTANGLES 128

/* The GE pads every vertex to the alignment of its largest component, here the
 * 32-bit colour, so this layout has a 12-byte stride: colour, x, y, z, then two
 * bytes of padding. The struct must not be packed. */
typedef struct {
    unsigned int color;
    short x, y, z;
} FlatVertex;
typedef char FlatVertexMustMatchGuStride[(sizeof(FlatVertex) == 12u) ? 1 : -1];

static unsigned int __attribute__((aligned(16))) display_list[65536];
/* The frame uses at most 125 rectangles: 48 playfield objects and 77 score segments. */
static FlatVertex rectangle_vertices[MAX_RECTANGLES * 2] __attribute__((aligned(16)));
static unsigned int rectangle_vertex_count;
static unsigned char bricks[BRICK_ROWS][BRICK_COLS];
static int high_score;

static void rect(int x, int y, int width, int height, unsigned int color) {
    FlatVertex *rectangle = &rectangle_vertices[rectangle_vertex_count];
    rectangle_vertex_count += 2;
    rectangle[0].color = rectangle[1].color = color;
    rectangle[0].x = (short)x;
    rectangle[0].y = (short)y;
    rectangle[0].z = 0;
    rectangle[1].x = (short)(x + width);
    rectangle[1].y = (short)(y + height);
    rectangle[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT |
                   GU_TRANSFORM_2D, 2, 0, rectangle);
}

static void draw_digit(int digit, int x, int y, unsigned int color) {
    static const unsigned char segments[10] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66,
        0x6d, 0x7d, 0x07, 0x7f, 0x6f
    };
    unsigned char bits = segments[digit % 10];
    if (bits & 0x01) rect(x + 2, y, 12, 3, color);
    if (bits & 0x02) rect(x + 14, y + 2, 3, 12, color);
    if (bits & 0x04) rect(x + 14, y + 17, 3, 12, color);
    if (bits & 0x08) rect(x + 2, y + 28, 12, 3, color);
    if (bits & 0x10) rect(x - 1, y + 17, 3, 12, color);
    if (bits & 0x20) rect(x - 1, y + 2, 3, 12, color);
    if (bits & 0x40) rect(x + 2, y + 14, 12, 3, color);
}

static void draw_number(int value, int x, int y, int digits, unsigned int color) {
    int divisor = 1;
    for (int i = 1; i < digits; i++) divisor *= 10;
    for (int i = 0; i < digits; i++) {
        draw_digit((value / divisor) % 10, x + i * 22, y, color);
        divisor /= 10;
    }
}

static void setup_graphics(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, FRAME_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)0x88000, FRAME_WIDTH);
    sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

static void savedata_request(PspUtilitySavedataMode mode, int *record) {
    SceUtilitySavedataParam params;
    memset(&params, 0, sizeof(params));
    params.base.size = sizeof(params);
    params.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    params.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
    params.base.graphicsThread = 17;
    params.base.accessThread = 19;
    params.base.fontThread = 18;
    params.base.soundThread = 16;
    params.mode = mode;
    strcpy(params.gameName, SAVE_GAME_NAME);
    strcpy(params.saveName, SAVE_NAME);
    strcpy(params.fileName, "SCORE.BIN");
    params.dataBuf = record;
    params.dataBufSize = sizeof(*record);
    params.dataSize = sizeof(*record);
    strcpy(params.sfoParam.title, "Nakagawa Showcase");
    strcpy(params.sfoParam.savedataTitle, "Breakout High Score");
    strcpy(params.sfoParam.detail, "A source-owned arcade demo.");
    sceUtilitySavedataInitStart(&params);
    int shutdown_started = 0;
    for (;;) {
        int status = sceUtilitySavedataGetStatus();
        if (status == PSP_UTILITY_DIALOG_NONE) break;
        if (!shutdown_started && (status == PSP_UTILITY_DIALOG_FINISHED ||
                                  status == PSP_UTILITY_DIALOG_QUIT)) {
            sceUtilitySavedataShutdownStart();
            shutdown_started = 1;
        }
        sceUtilitySavedataUpdate(1);
    }
}

static void draw_scene(int paddle_x, int ball_x, int ball_y, int score, int best, int lives) {
    sceGuStart(GU_DIRECT, display_list);
    rectangle_vertex_count = 0;
    sceGuClearColor(0xFF201810);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    rect(0, 48, SCREEN_WIDTH, 2, 0xFF5C4A36);
    rect(paddle_x, 246, 64, 10, 0xFFFFD840);
    rect(ball_x - 5, ball_y - 5, 10, 10, 0xFFFFFFFF);
    for (int row = 0; row < BRICK_ROWS; row++) {
        for (int col = 0; col < BRICK_COLS; col++) {
            if (bricks[row][col]) {
                unsigned int color = (row & 1) ? 0xFF756BE6 : 0xFF57C8FF;
                rect(26 + col * 48, 64 + row * 20, 42, 14, color);
            }
        }
    }
    draw_number(score, 30, 12, 5, 0xFFFFFFFF);
    draw_number(best, 218, 12, 5, 0xFF72E564);
    draw_number(lives, 420, 12, 1, 0xFF57C8FF);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

static void reset_bricks(void) {
    memset(bricks, 1, sizeof(bricks));
}

int main(void) {
    SceCtrlData pad;
    int paddle_x = 208;
    int ball_x = 240, ball_y = 226;
    int dx = 2, dy = -2;
    int score = 0, lives = 3;
    int audio_ready;
    unsigned int old_buttons = 0;

    setup_graphics();
    showcase_audio_init();
    audio_ready = showcase_audio_channel >= 0;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    memset(&pad, 0, sizeof(pad));
    reset_bricks();
    high_score = 0;
    savedata_request(PSP_UTILITY_SAVEDATA_AUTOLOAD, &high_score);
    if (high_score < 0) high_score = 0;

    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        if (pad.Lx < 120) paddle_x -= (120 - pad.Lx) / 10 + 1;
        if (pad.Lx > 136) paddle_x += (pad.Lx - 136) / 10 + 1;
        if (pad.Buttons & PSP_CTRL_LEFT) paddle_x -= 4;
        if (pad.Buttons & PSP_CTRL_RIGHT) paddle_x += 4;
        if (paddle_x < 8) paddle_x = 8;
        if (paddle_x > SCREEN_WIDTH - 72) paddle_x = SCREEN_WIDTH - 72;

        if (pad.Buttons & PSP_CTRL_START) {
            if (score > high_score) high_score = score;
            savedata_request(PSP_UTILITY_SAVEDATA_AUTOSAVE, &high_score);
            sceKernelExitGame();
        }
        if ((pad.Buttons & PSP_CTRL_CROSS) && !(old_buttons & PSP_CTRL_CROSS)) {
            if (lives <= 0) {
                lives = 3;
                score = 0;
                reset_bricks();
                ball_x = 240;
                ball_y = 226;
                dx = 2;
                dy = -2;
            } else if (audio_ready) {
                showcase_tone(880);
            }
        }
        old_buttons = pad.Buttons;

        if (lives > 0) {
            ball_x += dx;
            ball_y += dy;
            if (ball_x <= 5 || ball_x >= SCREEN_WIDTH - 5) dx = -dx;
            if (ball_y <= 54) dy = -dy;
            if (dy > 0 && ball_y + 5 >= 246 && ball_y <= 256 &&
                ball_x >= paddle_x - 5 && ball_x <= paddle_x + 69) {
                dy = -dy;
                dx = ((ball_x - (paddle_x + 32)) / 9);
                if (dx == 0) dx = 1;
                if (audio_ready) showcase_tone(440);
            }
            for (int row = 0; row < BRICK_ROWS; row++) {
                for (int col = 0; col < BRICK_COLS; col++) {
                    int left = 26 + col * 48;
                    int top = 64 + row * 20;
                    if (bricks[row][col] && ball_x >= left - 5 && ball_x <= left + 47 &&
                        ball_y >= top - 5 && ball_y <= top + 19) {
                        bricks[row][col] = 0;
                        dy = -dy;
                        score += 10;
                        if (score > high_score) high_score = score;
                        if (audio_ready) showcase_tone(990);
                    }
                }
            }
            if (ball_y > SCREEN_HEIGHT) {
                lives--;
                ball_x = 240;
                ball_y = 226;
                dy = -2;
            }
        }
        draw_scene(paddle_x, ball_x, ball_y, score, high_score, lives);
    }
}
