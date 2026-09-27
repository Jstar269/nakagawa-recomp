/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include <stddef.h>
#include <stdint.h>

#if defined(__mips__)
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspctrl.h>
#include <pspaudio.h>
#include <pspiofilemgr.h>
#define PSP_GUEST_ADDRESS(value) ((void *)(unsigned int)(value))
#else
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
typedef int SceUID;
typedef int (*SceKernelThreadEntry)(SceSize, void *);
typedef struct {
    uint32_t TimeStamp;
    uint32_t Buttons;
    uint8_t Lx;
    uint8_t Ly;
    uint8_t reserved[6];
} SceCtrlData;

#define PSP_MODULE_INFO(...)
#define PSP_MAIN_THREAD_ATTR(...)
#define PSP_CTRL_MODE_ANALOG 1
#define PSP_CTRL_CROSS 0x4000u
#define PSP_CTRL_START 0x0008u
#define PSP_AUDIO_FORMAT_STEREO 0
#define PSP_AUDIO_VOLUME_MAX 0x8000
#define PSP_O_RDONLY 0x0001
#define PSP_O_WRONLY 0x0002
#define PSP_O_CREAT 0x0200
#define PSP_O_TRUNC 0x0400
#define GU_DIRECT 0
#define GU_PSM_8888 3
#define GU_SCISSOR_TEST 1
#define GU_COLOR_BUFFER_BIT 1
#define GU_TRIANGLES 3
#define GU_COLOR_8888 0
#define GU_VERTEX_16BIT 0
#define GU_TRANSFORM_2D 0
#define GU_DEPTH_TEST 7
#define GU_TEXTURE_2D 8
#define GU_TRUE 1
#define PSP_GUEST_ADDRESS(value) ((void *)(uintptr_t)(value))

int sceKernelCreateThread(const char *, SceKernelThreadEntry, int, int, SceUInt, void *);
int sceKernelStartThread(SceUID, SceSize, void *);
int sceKernelWaitThreadEnd(SceUID, SceUInt *);
int sceKernelDeleteThread(SceUID);
int sceKernelExitGame(void);
void sceGuInit(void);
void sceGuTerm(void);
void sceGuStart(int, void *);
void sceGuDrawBuffer(int, void *, int);
void sceGuDispBuffer(int, int, void *, int);
void sceGuOffset(int, int);
void sceGuViewport(int, int, int, int);
void sceGuScissor(int, int, int, int);
void sceGuEnable(int);
void sceGuDisable(int);
void sceGuClearColor(uint32_t);
void sceGuClear(uint32_t);
int sceGuFinish(void);
int sceGuSync(int, int);
int sceGuDisplay(int);
void sceGuDrawArray(int, int, int, const void *, const void *);
void *sceGuSwapBuffers(void);
int sceDisplayWaitVblankStart(void);
int sceCtrlSetSamplingCycle(int);
int sceCtrlSetSamplingMode(int);
int sceCtrlReadBufferPositive(SceCtrlData *, int);
int sceAudioChReserve(int, int, int);
int sceAudioChRelease(int);
int sceAudioOutputPannedBlocking(int, int, int, void *);
int sceIoMkdir(const char *, int);
SceUID sceIoOpen(const char *, int, int);
int sceIoWrite(SceUID, const void *, unsigned int);
int sceIoRead(SceUID, void *, unsigned int);
int sceIoClose(SceUID);
#endif

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define FRAME_WIDTH 512
#define AUDIO_FRAMES 512

PSP_MODULE_INFO("PROFILE_ZERO_GUEST", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

typedef struct {
    uint32_t color;
    int16_t x;
    int16_t y;
    int16_t z;
} ProfileZeroVertex;

static unsigned int display_list[4096] __attribute__((aligned(16)));
static ProfileZeroVertex triangle[3] __attribute__((aligned(16))) = {
    {0xFFFF4040u, 72, 204, 0},
    {0xFF40FF40u, 240, 56, 0},
    {0xFF4040FFu, 408, 204, 0},
};
static int16_t audio_samples[AUDIO_FRAMES * 2] __attribute__((aligned(16)));
static volatile unsigned int scheduler_vblanks;

static int bytes_equal(const char *left, const char *right, size_t size) {
    for (size_t index = 0; index < size; index++) {
        if (left[index] != right[index]) return 0;
    }
    return 1;
}

static int append_text(char *output, size_t capacity, size_t *used,
                       const char *text) {
    while (*text) {
        if (*used >= capacity) return 0;
        output[(*used)++] = *text++;
    }
    return 1;
}

static int append_unsigned(char *output, size_t capacity, size_t *used,
                           unsigned int value) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count) {
        if (*used >= capacity) return 0;
        output[(*used)++] = digits[--count];
    }
    return 1;
}

static int append_signed(char *output, size_t capacity, size_t *used,
                         int value) {
    unsigned int magnitude = (unsigned int)value;
    if (value < 0) {
        if (*used >= capacity) return 0;
        output[(*used)++] = '-';
        magnitude = 0u - magnitude;
    }
    return append_unsigned(output, capacity, used, magnitude);
}

static int vblank_worker(SceSize arg_size, void *arg_pointer) {
    (void)arg_size;
    (void)arg_pointer;
    for (unsigned int frame = 0; frame < 2; frame++) {
        if (sceDisplayWaitVblankStart() < 0) return 1;
        scheduler_vblanks++;
    }
    return 0;
}

static int run_scheduler_probe(void) {
    SceUID worker = sceKernelCreateThread("profile-zero-vblank", vblank_worker,
                                         32, 0x1000, 0, NULL);
    if (worker < 0) return 0;
    int started = sceKernelStartThread(worker, 0, NULL);
    int waited = started >= 0 ? sceKernelWaitThreadEnd(worker, NULL) : -1;
    int deleted = sceKernelDeleteThread(worker);
    return started >= 0 && waited >= 0 && deleted >= 0 && scheduler_vblanks == 2;
}

static int submit_audio_sample(void) {
    int channel = sceAudioChReserve(-1, AUDIO_FRAMES, PSP_AUDIO_FORMAT_STEREO);
    if (channel < 0) return channel;
    for (unsigned int frame = 0; frame < AUDIO_FRAMES; frame++) {
        int16_t sample = (frame & 16u) ? 3000 : -3000;
        audio_samples[frame * 2] = sample;
        audio_samples[frame * 2 + 1] = sample;
    }
    int result = sceAudioOutputPannedBlocking(channel, PSP_AUDIO_VOLUME_MAX,
                                              PSP_AUDIO_VOLUME_MAX, audio_samples);
    sceAudioChRelease(channel);
    return result;
}

static int save_roundtrip(void) {
    static const char path[] = "ms0:/PROFILE_ZERO/ROUNDTRIP.BIN";
    static const char expected[] = "PROFILE_ZERO_SAVE_V1\n";
    char actual[sizeof(expected)];
    if (sceIoMkdir("ms0:/PROFILE_ZERO", 0777) < 0) return 0;
    SceUID file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (file < 0) return 0;
    int written = sceIoWrite(file, expected, (unsigned int)(sizeof(expected) - 1));
    int write_closed = sceIoClose(file);
    if (written != (int)(sizeof(expected) - 1) || write_closed < 0) return 0;
    file = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (file < 0) return 0;
    int read_count = sceIoRead(file, actual, (unsigned int)(sizeof(expected) - 1));
    int read_closed = sceIoClose(file);
    return read_count == (int)(sizeof(expected) - 1) && read_closed >= 0 &&
           bytes_equal(actual, expected, sizeof(expected) - 1);
}

static int write_result(unsigned int frames, unsigned int saw_cross,
                        unsigned int saw_start, int audio_result,
                        int scheduler_ok, int save_ok) {
    char result[256];
    size_t size = 0;
    (void)audio_result;
    if (!append_text(result, sizeof(result), &size, "ENTRY=1\nEXIT=0\nFRAMES=") ||
        !append_unsigned(result, sizeof(result), &size, frames) ||
        !append_text(result, sizeof(result), &size, "\nSCHEDULER_VBLANKS=") ||
        !append_unsigned(result, sizeof(result), &size, scheduler_vblanks) ||
        !append_text(result, sizeof(result), &size, "\nSCHEDULER_OK=") ||
        !append_unsigned(result, sizeof(result), &size, (unsigned int)scheduler_ok) ||
        !append_text(result, sizeof(result), &size, "\nINPUT_CROSS=") ||
        !append_unsigned(result, sizeof(result), &size, saw_cross) ||
        !append_text(result, sizeof(result), &size, "\nINPUT_START=") ||
        !append_unsigned(result, sizeof(result), &size, saw_start) ||
        !append_text(result, sizeof(result), &size, "\nAUDIO_RESULT=") ||
        !append_signed(result, sizeof(result), &size, audio_result) ||
        !append_text(result, sizeof(result), &size, "\nGE_PRIMITIVES=") ||
        !append_unsigned(result, sizeof(result), &size, frames) ||
        !append_text(result, sizeof(result), &size, "\nSAVE_ROUNDTRIP=") ||
        !append_unsigned(result, sizeof(result), &size, (unsigned int)save_ok) ||
        !append_text(result, sizeof(result), &size, "\n")) return 0;
    SceUID file = sceIoOpen("ms0:/PROFILE_ZERO/RESULT.TXT",
                            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (file < 0) return 0;
    int written = sceIoWrite(file, result, (unsigned int)size);
    int closed = sceIoClose(file);
    return written >= 0 && (size_t)written == size && closed >= 0;
}

static void draw_frame(void) {
    sceGuStart(GU_DIRECT, display_list);
    sceGuClearColor(0xFF201810u);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuDrawArray(GU_TRIANGLES,
                   GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D,
                   3, NULL, triangle);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

int main(void) {
    SceCtrlData pad = {0};
    unsigned int frames = 0;
    unsigned int saw_cross = 0;
    unsigned int saw_start = 0;
    unsigned int old_buttons = 0;
    int audio_result = -1;

    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);
    sceGuDrawBuffer(GU_PSM_8888, PSP_GUEST_ADDRESS(0), FRAME_WIDTH);
    sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT,
                    PSP_GUEST_ADDRESS(0x88000u), FRAME_WIDTH);
    sceGuOffset(2048 - SCREEN_WIDTH / 2, 2048 - SCREEN_HEIGHT / 2);
    sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuClearColor(0xFF201810u);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    int scheduler_ok = run_scheduler_probe();
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    while (!saw_start) {
        sceCtrlReadBufferPositive(&pad, 1);
        if ((pad.Buttons & PSP_CTRL_CROSS) && !(old_buttons & PSP_CTRL_CROSS)) {
            saw_cross = 1;
            audio_result = submit_audio_sample();
        }
        if (pad.Buttons & PSP_CTRL_START) {
            saw_start = 1;
            break;
        }
        old_buttons = pad.Buttons;
        draw_frame();
        frames++;
    }

    sceGuTerm();
    int save_ok = save_roundtrip();
    write_result(frames, saw_cross, saw_start, audio_result, scheduler_ok, save_ok);
    sceKernelExitGame();
    return 0;
}
