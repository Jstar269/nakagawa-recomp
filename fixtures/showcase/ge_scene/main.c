/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspctrl.h>
#include <string.h>

#include "../common/tone.h"

PSP_MODULE_INFO("NAKAGAWA_SHOWCASE_3D", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define FRAME_WIDTH 512

typedef struct {
    float u, v;
    float nx, ny, nz;
    float x, y, z;
} SceneVertex;

static unsigned int __attribute__((aligned(16))) display_list[262144];
static unsigned short __attribute__((aligned(16))) checker[64 * 64];

#define V(u_, v_, nx_, ny_, nz_, x_, y_, z_) \
    { (u_), (v_), (nx_), (ny_), (nz_), (x_), (y_), (z_) }

static const SceneVertex cube_faces[6][6] = {
    { V(0,0, 0,0,1,-0.8f,-0.8f,0.8f), V(64,0, 0,0,1,0.8f,-0.8f,0.8f), V(64,64, 0,0,1,0.8f,0.8f,0.8f), V(0,0, 0,0,1,-0.8f,-0.8f,0.8f), V(64,64, 0,0,1,0.8f,0.8f,0.8f), V(0,64, 0,0,1,-0.8f,0.8f,0.8f) },
    { V(0,0, 0,0,-1,0.8f,-0.8f,-0.8f), V(64,0, 0,0,-1,-0.8f,-0.8f,-0.8f), V(64,64, 0,0,-1,-0.8f,0.8f,-0.8f), V(0,0, 0,0,-1,0.8f,-0.8f,-0.8f), V(64,64, 0,0,-1,-0.8f,0.8f,-0.8f), V(0,64, 0,0,-1,0.8f,0.8f,-0.8f) },
    { V(0,0, 1,0,0,0.8f,-0.8f,0.8f), V(64,0, 1,0,0,0.8f,-0.8f,-0.8f), V(64,64, 1,0,0,0.8f,0.8f,-0.8f), V(0,0, 1,0,0,0.8f,-0.8f,0.8f), V(64,64, 1,0,0,0.8f,0.8f,-0.8f), V(0,64, 1,0,0,0.8f,0.8f,0.8f) },
    { V(0,0, -1,0,0,-0.8f,-0.8f,-0.8f), V(64,0, -1,0,0,-0.8f,-0.8f,0.8f), V(64,64, -1,0,0,-0.8f,0.8f,0.8f), V(0,0, -1,0,0,-0.8f,-0.8f,-0.8f), V(64,64, -1,0,0,-0.8f,0.8f,0.8f), V(0,64, -1,0,0,-0.8f,0.8f,-0.8f) },
    { V(0,0, 0,1,0,-0.8f,0.8f,0.8f), V(64,0, 0,1,0,0.8f,0.8f,0.8f), V(64,64, 0,1,0,0.8f,0.8f,-0.8f), V(0,0, 0,1,0,-0.8f,0.8f,0.8f), V(64,64, 0,1,0,0.8f,0.8f,-0.8f), V(0,64, 0,1,0,-0.8f,0.8f,-0.8f) },
    { V(0,0, 0,-1,0,-0.8f,-0.8f,-0.8f), V(64,0, 0,-1,0,0.8f,-0.8f,-0.8f), V(64,64, 0,-1,0,0.8f,-0.8f,0.8f), V(0,0, 0,-1,0,-0.8f,-0.8f,-0.8f), V(64,64, 0,-1,0,0.8f,-0.8f,0.8f), V(0,64, 0,-1,0,-0.8f,-0.8f,0.8f) }
};

static void make_checker(void) {
    for (int y = 0; y < 64; y++) {
        for (int x = 0; x < 64; x++) {
            int band = ((x / 8) ^ (y / 8)) & 1;
            checker[y * 64 + x] = band ? 0x7FE0 : 0xFC1F;
        }
    }
}

static void setup_graphics(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, FRAME_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)0x88000, FRAME_WIDTH);
    sceGuDepthBuffer((void *)0x110000, FRAME_WIDTH);
    sceGuOffset(2048 - (SCREEN_WIDTH / 2), 2048 - (SCREEN_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_DEPTH_TEST);
    sceGuDepthFunc(GU_GEQUAL);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexImage(0, 64, 64, 64, checker);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuEnable(GU_LIGHTING);
    sceGuEnable(GU_LIGHT0);
    sceGuLightMode(GU_SINGLE_COLOR);
    {
        ScePspFVector3 direction = { 0.25f, 0.75f, 1.0f };
        sceGuLight(0, GU_DIRECTIONAL, GU_DIFFUSE_AND_SPECULAR, &direction);
    }
    sceGuLightColor(0, GU_DIFFUSE, 0xFFFFFFFF);
    sceGuAmbientColor(0x444444);
    sceGuColor(0xFFFFFFFF);
    sceGuClearColor(0xFF201810);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

int main(void) {
    SceCtrlData pad;
    ScePspFVector3 translation = { 0.0f, 0.0f, -3.2f };
    ScePspFVector3 rotation = { 20.0f, 30.0f, 0.0f };
    unsigned int old_buttons = 0;
    int audio_ready;

    make_checker();
    setup_graphics();
    showcase_audio_init();
    audio_ready = showcase_audio_channel >= 0;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    memset(&pad, 0, sizeof(pad));

    for (;;) {
        sceCtrlReadBufferPositive(&pad, 1);
        rotation.y += 0.5f;
        rotation.y += ((int)pad.Lx - 128) * 0.06f;
        rotation.x += ((int)pad.Ly - 128) * 0.06f;
        if ((pad.Buttons & PSP_CTRL_CROSS) && !(old_buttons & PSP_CTRL_CROSS) && audio_ready) {
            showcase_tone(660);
        }
        old_buttons = pad.Buttons;
        if (pad.Buttons & PSP_CTRL_START) break;

        sceGuStart(GU_DIRECT, display_list);
        sceGuClearColor(0xFF201810);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGumMatrixMode(GU_PROJECTION);
        sceGumLoadIdentity();
        sceGumPerspective(55.0f, 16.0f / 9.0f, 0.5f, 100.0f);
        sceGumMatrixMode(GU_VIEW);
        sceGumLoadIdentity();
        sceGumMatrixMode(GU_MODEL);
        sceGumLoadIdentity();
        sceGumTranslate(&translation);
        sceGumRotateXYZ(&rotation);
        sceGumUpdateMatrix();
        for (int face = 0; face < 6; face++) {
            sceGuDrawArray(GU_TRIANGLES,
                GU_TEXTURE_32BITF | GU_NORMAL_32BITF | GU_VERTEX_32BITF |
                    GU_TRANSFORM_3D,
                6, 0, cube_faces[face]);
        }
        sceGuFinish();
        sceGuSync(0, 0);
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
