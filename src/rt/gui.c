// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

/* * to sceCtrl, so the recompiled game runs like a normal app (watch the intro, press START, play).
 *
 * The default presenter is SDL3 + Vulkan (src/rt/gpu_sdl3vk), which owns the window and ALL input
 * devices — the comprehensive SDL3 gamepad subsystem plus an SDL keyboard fallback. gui.c simply
 * forwards that state to the runtime. When SR_VIDEO=gdi selects the legacy Win32/GDI window
 * (a top-level window + a 32-bit DIB the PSP framebuffer is converted into each frame), input
 * falls back to a keyboard-only GetAsyncKeyState mapping; controllers on that debug path are not
 * supported (no XInput/DirectInput dependency). This whole file is Windows-only (#ifdef _WIN32);
 * on other platforms the SDL3 layer provides the window and input directly. */

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#ifdef SR_SDL3VK
#include "gpu_sdl3vk/sdl3vk.h"
#include "gpu_sdl3vk/ge_gpu.h"
#endif
#include <windows.h>
#include <SDL3/SDL_timer.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PSP_W 480
#define PSP_H 272

static int      s_on = 0;
static int      s_sdl3 = 0;            /* SDL3+Vulkan presenter active (src/rt/gpu_sdl3vk) */
static HWND     s_hwnd;
static uint32_t *s_px;                 /* PSP_W*PSP_H BGRA for StretchDIBits */
static BITMAPINFO s_bmi;
static uint32_t s_buttons = 0;
static uint8_t  s_lx = 128, s_ly = 128;   /* live left-stick (0..255, 128=centre), latched each present */
static int      s_pad_present = 0;         /* a controller is currently connected */
static uint64_t s_last_ns;             /* fallback frame-pacing deadline */
static uint64_t s_present_next_ns;      /* 30 Hz output cap; never delays guest execution */
static int      s_present_cap = -1;

/* Hot Shots Tennis is a 30 FPS title on a ~59.94 Hz PSP display. The guest can call
 * sceDisplaySetFrameBuf repeatedly within one scanout interval; presenting every call
 * wastes GPU/WSI work and produced 90+ host presents/s in the title path. Drop only
 * calls that arrive before the next output slot. Input still gets pumped by the caller,
 * and a scene already below the cap is never delayed. SR_FPS_CAP=0 disables this for
 * diagnostics; other positive values are accepted for controlled experiments. */
static int present_slot_due(void) {
    if (s_present_cap < 0) {
        const char *value = getenv("SR_FPS_CAP");
        s_present_cap = value ? atoi(value) : 30;
        if (s_present_cap < 0) s_present_cap = 0;
        if (s_present_cap > 240) s_present_cap = 240;
    }
    if (s_present_cap == 0) return 1;

    uint64_t now = SDL_GetTicksNS();
    uint64_t period = 1000000000ull / (uint64_t)s_present_cap;
    if (s_present_next_ns && now < s_present_next_ns) return 0;
    if (!s_present_next_ns || now > s_present_next_ns + period * 4u) {
        s_present_next_ns = now + period;
    } else {
        do { s_present_next_ns += period; } while (s_present_next_ns <= now);
    }
    return 1;
}

/* True when the most recent gui_present() call actually attempted a host present (the
 * output cap did not drop it). hle.c uses this to run the present-gap watchdog. */
static int s_last_present_attempted = 0;
int gui_present_attempted(void) { return s_last_present_attempted; }

#ifdef SR_SDL3VK
static void sync_sdl_input(void) {
    s_buttons = sdl3vk_buttons();
    sdl3vk_analog(&s_lx, &s_ly);
    s_pad_present = sdl3vk_pad_present();
}
#endif

/* PSP button bits (sceCtrl): SELECT 0x1, START 0x8, UP 0x10, RIGHT 0x20, DOWN 0x40, LEFT 0x80,
 * LTRIG 0x100, RTRIG 0x200, TRIANGLE 0x1000, CIRCLE 0x2000, CROSS 0x4000, SQUARE 0x8000. */
static uint32_t read_keys(void) {
    uint32_t b = 0;
    #define K(vk,bit) do { if (GetAsyncKeyState(vk) & 0x8000) b |= (bit); } while (0)
    K(VK_RETURN, 0x0008);              /* Enter  -> START  */
    K(VK_RSHIFT, 0x0001); K(VK_LSHIFT, 0x0001); /* Shift -> SELECT */
    K('X', 0x4000);                    /* X -> CROSS   (confirm) */
    K('Z', 0x2000);                    /* Z -> CIRCLE  (back)    */
    K('A', 0x8000);                    /* A -> SQUARE  */
    K('S', 0x1000);                    /* S -> TRIANGLE */
    K('Q', 0x0100);                    /* Q -> L */
    K('W', 0x0200);                    /* W -> R */
    K(VK_UP, 0x0010); K(VK_DOWN, 0x0040); K(VK_LEFT, 0x0080); K(VK_RIGHT, 0x0020);
    #undef K
    return b;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}

void gui_init(const char *title) {
#ifdef SR_SDL3VK
    /* SDL3+Vulkan presenter (src/rt/gpu_sdl3vk, Phase 0): default in this build;
     * SR_VIDEO=gdi falls back to the classic Win32/GDI window below. */
    {
        const char *v = getenv("SR_VIDEO");
        if (!v || strcmp(v, "gdi") != 0) {
            if (sdl3vk_init(title)) {
                s_sdl3 = 1;
                s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
                s_last_ns = SDL_GetTicksNS();
                s_on = 1;
                sync_sdl_input();
                /* Phase 1 GPU rasterizer (opt-in): captures GE triangles/sprites and
                 * renders them on the GPU, writing results back to guest VRAM. */
                {
                    const char *gge = getenv("SR_GPU_GE");
                    if (gge && gge[0] && strcmp(gge, "0") != 0) {
                        if (!gegpu_init())
                            fprintf(stderr, "gui_init: GPU GE init failed; software GE active\n");
                    }
                }
                fprintf(stderr, "BOOT_EVENT phase=window_ready backend=vulkan\n");
                return;
            }
            fprintf(stderr, "gui_init: SDL3/Vulkan init failed; falling back to GDI\n");
        }
    }
#endif
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "psp_recomp";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, PSP_W * 2, PSP_H * 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    s_hwnd = CreateWindowA("psp_recomp", title ? title : SR_APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top, 0, 0, GetModuleHandleA(0), 0);
    s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
    s_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    s_bmi.bmiHeader.biWidth = PSP_W;
    s_bmi.bmiHeader.biHeight = -PSP_H;          /* top-down */
    s_bmi.bmiHeader.biPlanes = 1;
    s_bmi.bmiHeader.biBitCount = 32;
    s_bmi.bmiHeader.biCompression = BI_RGB;
    s_last_ns = SDL_GetTicksNS();
    s_on = 1;
    fprintf(stderr, "BOOT_EVENT phase=window_ready backend=gdi\n");
}

int gui_on(void) { return s_on; }
uint32_t gui_buttons(void) { return s_buttons; }
void gui_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = s_lx; if (ly) *ly = s_ly; }
int gui_pad_present(void) { return s_pad_present; }
void gui_consume_button_pulses(void) {
#ifdef SR_SDL3VK
    if (s_sdl3) {
        sdl3vk_consume_button_pulses();
        sync_sdl_input();
    }
#endif
}

/* Present a framebuffer at guest address fbaddr. fmt: 0=5650, 1=5551, 2=4444, 3=8888.
 * stride is in pixels (PSP buffer width, typically 512). */
/* Convert the guest framebuffer to the BGRA words both presenters consume. */
static void convert_fb(uint32_t fbaddr, int fmt, uint32_t stride) {
    for (int y = 0; y < PSP_H; y++) {
        for (int x = 0; x < PSP_W; x++) {
            uint32_t i = (uint32_t)(y * (int)stride + x);
            int rr, gg, bb;
            if (fmt == 3) {
                uint32_t p = sr_r32(fbaddr + i * 4);
                rr = p & 0xFF; gg = (p >> 8) & 0xFF; bb = (p >> 16) & 0xFF;
            } else {
                uint16_t p = sr_r16(fbaddr + i * 2);
                if (fmt == 1) {            /* 5551 */
                    rr = (p & 0x1F) << 3; gg = ((p >> 5) & 0x1F) << 3; bb = ((p >> 10) & 0x1F) << 3;
                } else if (fmt == 2) {     /* 4444 */
                    rr = (p & 0xF) << 4; gg = ((p >> 4) & 0xF) << 4; bb = ((p >> 8) & 0xF) << 4;
                } else {                   /* 5650 */
                    rr = (p & 0x1F) << 3; gg = ((p >> 5) & 0x3F) << 2; bb = ((p >> 11) & 0x1F) << 3;
                }
            }
            s_px[y * PSP_W + x] = ((uint32_t)rr << 16) | ((uint32_t)gg << 8) | (uint32_t)bb;
        }
    }
    static int s_first_frame = 1;
    if (s_first_frame) {
        s_first_frame = 0;
        unsigned long nz = 0;
        for (int yy = 0; yy < PSP_H; yy++)
            for (int xx = 0; xx < PSP_W; xx++)
                if (s_px[yy * PSP_W + xx] & 0x00FFFFFF) nz++;
        fprintf(stderr, "[FIRST_FRAME] non_zero_pixels=%lu / total=%u\n", nz, PSP_W * PSP_H);
        fprintf(stderr, "BOOT_EVENT phase=first_frame source=cpu nonzero_pixels=%lu total_pixels=%u\n",
                nz, PSP_W * PSP_H);
        if (getenv("SR_FIRST_FRAME_DUMP")) {
            FILE *raw = fopen("vram_first.bin", "wb");
            if (raw) {
                for (uint32_t a = 0x04000000; a < 0x04000000u + 480u * 272u * 2u; a++)
                    fputc((int)sr_r8(a), raw);
                fclose(raw);
                fprintf(stderr, "[FIRST_FRAME] dumped VRAM 0x04000000 -> vram_first.bin\n");
            }
            FILE *ppm = fopen("frame_first.ppm", "wb");
            if (ppm) {
                fprintf(ppm, "P6\n480 272\n255\n");
                for (int yy = 0; yy < PSP_H; yy++)
                    for (int xx = 0; xx < PSP_W; xx++) {
                        uint32_t p = s_px[yy * PSP_W + xx];
                        unsigned char rgb[3] = { (unsigned char)((p >> 16) & 0xFF),
                                                (unsigned char)((p >> 8) & 0xFF),
                                                (unsigned char)(p & 0xFF) };
                        fwrite(rgb, 1, 3, ppm);
                    }
                fclose(ppm);
                fprintf(stderr, "[FIRST_FRAME] dumped BGRA -> frame_first.ppm\n");
            }
        }
    }
}

void gui_pump(void) {
    if (!s_on) return;
#ifdef SR_SDL3VK
    if (s_sdl3) {
        int alive = sdl3vk_poll();
        sync_sdl_input();
        if (!alive) { _Exit(0); }
        return;
    }
#endif
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { _Exit(0); }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

void gui_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    s_last_present_attempted = 0;
    if (!s_on) return;
    if (stride == 0) stride = 512;

    if (!present_slot_due()) {
        sr_perf_present_skip();
#ifdef SR_SDL3VK
        /* A present that never runs must not service an armed capture: otherwise the
         * stale arm is picked up by a LATER present and publishes frame N's file name
         * with frame N+k's pixels (and blocks every arm in between). Resolve it as
         * "nothing attempted" so the next present can arm cleanly. */
        if (s_sdl3) sdl3vk_capture_cancel();
#endif
        gui_pump();
        return;
    }
    s_last_present_attempted = 1;

#ifdef SR_SDL3VK
    if (s_sdl3) {
        /* 1. GPU VRAM image fast path: any frame the GPU rasterizer produced is presented
         *    directly from its Vulkan image. Returns 1 on success (skip GDI), -1 when the
         *    address isn't GPU-resident (CPU movie frames etc.) — fall through to BGRA. */
        int res = gegpu_present(fbaddr, fmt, stride);
        sync_sdl_input();
        if (res == 0) { _Exit(0); }
        if (res == 1) {
            /* The GPU fast path never reaches convert_fb, which is the only emitter of
             * the first_frame boot milestone. Without this, boot_gate.py can never pass
             * on the default renderer even while the GPU presents every frame. There is
             * no host-side copy of these pixels to count, so the event reports its
             * source instead of a nonzero-pixel total; the gate accepts source=gpu as
             * proof a real frame reached the swapchain. */
            static int s_first_gpu_frame = 1;
            if (s_first_gpu_frame) {
                s_first_gpu_frame = 0;
                fprintf(stderr, "BOOT_EVENT phase=first_frame source=gpu presented=1\n");
            }
            goto pace;
        }

        /* 2. CPU-rendered fallback: convert guest VRAM to BGRA and blit through SDL3/Vulkan. */
        convert_fb(fbaddr, fmt, stride);
        int present_result = sdl3vk_present_rgba(s_px);
        sync_sdl_input();
        if (present_result == 0) { _Exit(0); }
        goto pace;
    }
#endif

    gui_pump();
    /* GDI fallback is keyboard-only; controllers are handled by the SDL3 gamepad subsystem
     * (src/rt/gpu_sdl3vk) on the default Vulkan path. */
    s_buttons = read_keys();
    s_lx = 128; s_ly = 128;
    s_pad_present = 0;

    convert_fb(fbaddr, fmt, stride);
    HDC dc = GetDC(s_hwnd);
    RECT cr; GetClientRect(s_hwnd, &cr);
    StretchDIBits(dc, 0, 0, cr.right, cr.bottom, 0, 0, PSP_W, PSP_H,
                  s_px, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(s_hwnd, dc);

#ifdef SR_SDL3VK
pace:
#endif
    /* Pace to ~60 Hz so the intro/menus play at a watchable speed. Skipped when the scheduler
     * already paces vblanks to real time (the default): pacing twice on unaligned grids makes
     * frames miss their vblank and costs a whole extra period (30/20 fps quantization). */
    {
        extern int sched_vbl_paced(void);
        if (sched_vbl_paced()) { s_last_ns = SDL_GetTicksNS(); return; }
    }
    uint64_t now_ns = SDL_GetTicksNS();
    const uint64_t period_ns = 1000000000u / 60u;
    if (now_ns - s_last_ns < period_ns)
        SDL_DelayPrecise(period_ns - (now_ns - s_last_ns));
    s_last_ns = SDL_GetTicksNS();
}
#endif
