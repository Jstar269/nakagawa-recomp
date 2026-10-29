// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

/* The game's window: presents the PSP framebuffer each frame and feeds keyboard and gamepad
 * input to sceCtrl, so the recompiled game runs like a normal app (watch the intro, press START,
 * play). gui_init() picks the presenter, gui_present() paces and shows a frame, and the
 * gui_pause_* functions run the pause overlay.
 *
 * The default presenter is SDL3 + Vulkan (src/rt/gpu_sdl3vk), which owns the window and ALL input
 * devices — the comprehensive SDL3 gamepad subsystem plus an SDL keyboard fallback. gui.c simply
 * forwards that state to the runtime. When SR_VIDEO=gdi selects the legacy Win32/GDI window
 * (a top-level window + a 32-bit DIB the PSP framebuffer is converted into each frame), input
 * falls back to a keyboard-only GetAsyncKeyState mapping; controllers on that debug path are not
 * supported (no XInput/DirectInput dependency).
 * SR_VIDEO=offscreen is an explicit host-memory sink for headless bring-up: it converts validated
 * guest frames without initializing SDL video, creating a window, or claiming a presentable
 * Vulkan surface, and it provides no live input device.
 *
 * The SDL3/Vulkan presenter is host-neutral and is the only presenter on a host without
 * Win32/GDI: everything GDI-shaped (the window class, the DIB, GetAsyncKeyState, the message
 * pump) is behind _WIN32, and a host without it gets the SDL3 path or, if that cannot
 * initialise, a reported refusal (no window, s_on == 0) rather than a fabricated one. */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "gpu_sdl3vk/sdl3vk.h"
#include "fbcap.h"
#include "osk_overlay.h"        /* the in-window keyboard: keys in, the frame drawn over */
#include "osk_overlay_paint.h"
#ifdef SR_SDL3VK
#include "gpu_sdl3vk/ge_gpu.h"
#endif
#if defined(SR_SDL3VK) && !defined(SR_GUI_PRESENT_SELFTEST)
#include "nk_ui_theme.h"         /* the pause overlay's scrim token */
#include "overlay/overlay_host.h" /* the pause overlay's pages (C9c) */
#include "ge_shared.h"           /* the developer VRAM viewer's decoders (#314) */
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <stdint.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define PSP_W 480
#define PSP_H 272
#define PSP_SCANOUT_RATE_NUMERATOR 60000ull
#define PSP_SCANOUT_RATE_DENOMINATOR 1001ull
#define PSP_SCANOUT_PERIOD_NS \
    ((PSP_SCANOUT_RATE_DENOMINATOR * 1000000000ull + PSP_SCANOUT_RATE_NUMERATOR / 2u) / \
     PSP_SCANOUT_RATE_NUMERATOR)

enum {
    PRESENT_CAP_ENV_UNRESOLVED = -1,
    PRESENT_CAP_SCANOUT = -2
};

/* gui-present-selftest compiles this file alone and exercises only the host present
 * pacing below. The presenters need guest memory, the scheduler, perf telemetry and
 * the SDL3/Vulkan backend, so they stay out of that binary instead of being stubbed:
 * a pacing regression must not depend on a stubbed runtime. */
#ifndef SR_GUI_PRESENT_SELFTEST
static int      s_on = 0;
#ifdef SR_SDL3VK
static int      s_sdl3 = 0;            /* SDL3+Vulkan presenter active (src/rt/gpu_sdl3vk) */
#endif
#if defined(SR_SDL3VK) || defined(_WIN32)
static bool     s_foreground_request_issued;
#endif
static int      s_offscreen = 0;        /* explicit host-memory presenter for headless bring-up */
static uint64_t s_offscreen_frames;
static int      s_headless_accept_parsed;
static int      s_headless_accept_requested;
static int      s_headless_accept_invalid;
static int      s_headless_accept_ready;
static int      s_headless_accept_complete;
static uint32_t s_headless_accept_target;
static uint32_t s_headless_accept_presented;
#ifdef _WIN32
static HWND     s_hwnd;
static BITMAPINFO s_bmi;
#endif
static uint32_t *s_px;                 /* PSP_W*PSP_H BGRA for every presenter, including offscreen */
static uint32_t s_buttons = 0;
static uint8_t  s_lx = 128, s_ly = 128;   /* live left-stick (0..255, 128=centre), latched each present */
static int      s_pad_present = 0;         /* a controller is currently connected */
static uint64_t s_last_ns;             /* fallback frame-pacing deadline */
/* The framebuffer the guest last had displayed: a pause freezes its current contents. */
static struct { uint32_t addr; int fmt; uint32_t stride; int valid; } s_shown_fb;
#endif /* !SR_GUI_PRESENT_SELFTEST */
static uint64_t s_present_next_ns;      /* host output cap; never delays guest execution */
static int      s_present_cap = PRESENT_CAP_ENV_UNRESOLVED;

#ifndef SR_GUI_PRESENT_SELFTEST
/* Keep boot milestones observable to the parent player without depending on
 * whether the platform process backend inherits stderr. The file is opt-in and
 * is otherwise not opened by the runtime. */
static void sr_gui_boot_event(const char *format, ...) {
    uint64_t timestamp_ns = sr_perf_now_ns();
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    if (timestamp_ns) {
        fprintf(stderr, " t_ns=%llu\n", (unsigned long long)timestamp_ns);
    } else {
        fputc('\n', stderr);
    }
    fflush(stderr);

    const char *path = getenv("SR_BOOT_EVENT_FILE");
    if (!path || !*path) return;

    FILE *file = fopen(path, "ab");
    if (!file) return;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);
    if (timestamp_ns) {
        fprintf(file, " t_ns=%llu\n", (unsigned long long)timestamp_ns);
    } else {
        fputc('\n', file);
    }
    fclose(file);
}

static void gui_headless_acceptance_parse(void) {
    if (s_headless_accept_parsed) return;
    s_headless_accept_parsed = 1;
    const char *value = getenv("SR_HEADLESS_ACCEPT_PRESENTED_FRAMES");
    if (!value || !value[0]) return;
    s_headless_accept_requested = 1;
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno || end == value || !end || *end || parsed == 0u ||
        parsed > UINT32_MAX) {
        s_headless_accept_invalid = 1;
        return;
    }
    s_headless_accept_target = (uint32_t)parsed;
}

int gui_headless_acceptance_requested(void) {
    gui_headless_acceptance_parse();
    return s_headless_accept_requested;
}

int gui_headless_acceptance_ready(void) {
    gui_headless_acceptance_parse();
    return s_headless_accept_requested && !s_headless_accept_invalid &&
        s_headless_accept_ready;
}

int gui_headless_acceptance_complete(void) {
    return s_headless_accept_complete;
}

uint32_t gui_headless_acceptance_presented_frames(void) {
    return s_headless_accept_presented;
}

static void gui_headless_acceptance_record_present(int accepted) {
    if (!accepted || !s_headless_accept_requested ||
        !s_headless_accept_ready || s_offscreen ||
        s_headless_accept_complete) return;
    s_headless_accept_presented++;
    if (s_headless_accept_presented >= s_headless_accept_target) {
        s_headless_accept_complete = 1;
    }
}
#endif /* !SR_GUI_PRESENT_SELFTEST */

/* A guest may call sceDisplaySetFrameBuf several times during one scanout interval;
 * presenting more often than the display refresh wastes GPU/WSI work. The default
 * period uses the same 60000/1001 Hz scanout rate as the scheduler. */
static int present_cap_from_value(const char *value) {
    if (!value || !value[0] || strcmp(value, "native") == 0) {
        return PRESENT_CAP_SCANOUT;
    }
    char *end = NULL;
    long cap = strtol(value, &end, 10);
    if (end == value || *end != '\0' || cap < 0) return PRESENT_CAP_SCANOUT;
    if (cap > 240) return 240;
    return (int)cap;
}

static uint64_t present_period_for_cap(int cap) {
    if (cap == PRESENT_CAP_SCANOUT) return PSP_SCANOUT_PERIOD_NS;
    return cap ? 1000000000ull / (uint64_t)cap : 0;
}

static uint64_t present_period_ns(void) {
    if (s_present_cap == PRESENT_CAP_ENV_UNRESOLVED)
        s_present_cap = present_cap_from_value(getenv("SR_FPS_CAP"));
    return present_period_for_cap(s_present_cap);
}

/* Output slots sit on a host-time grid one period apart, its phase set by the first present.
 * A submission takes the slot nearest to it, so one that arrives up to half a period before
 * the next slot is still presented. Guest VBLANKs reach the host with scheduling jitter: with
 * a floor rule (present only once the slot has started), every flip that came a little ahead
 * of the grid's phase was dropped although it was the only flip in its scanout. The grid
 * still advances one period per present, so the long-run rate never exceeds the cap, and a
 * second submission inside the same slot is dropped. */
static int present_slot_due_at(uint64_t now) {
    uint64_t period = present_period_ns();
    if (!period) return 1;

    uint64_t early = period / 2u;
    if (s_present_next_ns && now + early < s_present_next_ns) return 0;
    if (!s_present_next_ns || now > s_present_next_ns + period * 4u) {
        s_present_next_ns = now + period;
    } else {
        do { s_present_next_ns += period; } while (s_present_next_ns <= now);
    }
    return 1;
}

static int present_slot_due(void) {
    return present_slot_due_at(SDL_GetTicksNS());
}

#ifndef SR_GUI_PRESENT_SELFTEST
#ifdef SR_SDL3VK
/* The window's keys and typed text go to the in-window keyboard while it is open. The text
 * input is claimed only then, so Escape opens the pause the rest of the time. */
static void sr_gui_feed_keyboard(void) {
    sdl3vk_set_text_claim(sr_osk_overlay_active());
    Sdl3VkKeyEvent ev[32];
    int n = sdl3vk_take_key_events(ev, 32);
    for (int i = 0; i < n; i++) {
        switch (ev[i].key) {
        case SDL3VK_KEY_UP:        sr_osk_overlay_key(OSK_KEY_UP); break;
        case SDL3VK_KEY_DOWN:      sr_osk_overlay_key(OSK_KEY_DOWN); break;
        case SDL3VK_KEY_LEFT:      sr_osk_overlay_key(OSK_KEY_LEFT); break;
        case SDL3VK_KEY_RIGHT:     sr_osk_overlay_key(OSK_KEY_RIGHT); break;
        case SDL3VK_KEY_ENTER:     sr_osk_overlay_key(OSK_KEY_SELECT); break;
        case SDL3VK_KEY_ESCAPE:    sr_osk_overlay_key(OSK_KEY_CANCEL); break;
        case SDL3VK_KEY_TAB:       sr_osk_overlay_key(OSK_KEY_CONFIRM); break;
        case SDL3VK_KEY_BACKSPACE: sr_osk_overlay_key(OSK_KEY_BACKSPACE); break;
        case SDL3VK_KEY_TEXT:      sr_osk_overlay_type(ev[i].codepoint); break;
        default: break;
        }
    }
}

/* Esc, F1 or Guide (or the open pad going away) asks the scheduler for a pause. The request
 * only latches: the pause opens at the scheduler's next switch point, and a refused request
 * leaves the game running (sched.c logs why). */
static void sr_gui_take_pause_request(void) {
    int request = sdl3vk_take_pause_request();
    if (request == SDL3VK_PAUSE_REQUEST_NONE || !sr_sched_on) return;
    (void)sched_pause_request(request == SDL3VK_PAUSE_REQUEST_PAD_LOST
                                  ? SR_PAUSE_SRC_PAD_LOST : SR_PAUSE_SRC_USER);
}

static void sync_sdl_input(void) {
    s_buttons = sdl3vk_buttons();
    sdl3vk_analog(&s_lx, &s_ly);
    s_pad_present = sdl3vk_pad_present();
    sr_gui_feed_keyboard();
    sr_gui_take_pause_request();
}
#endif

#if defined(SR_SDL3VK) || defined(_WIN32)
static bool sr_gui_is_headless_presenter(void) {
    const char *headless = getenv("SR_HEADLESS");
    if (headless && headless[0] && strcmp(headless, "0") != 0) return true;

    const char *driver = SDL_GetCurrentVideoDriver();
    return driver && (strcmp(driver, "dummy") == 0 ||
                      strcmp(driver, "offscreen") == 0);
}

#ifdef SR_SDL3VK
static void sr_gui_raise_sdl_window(void *context) {
    (void)context;
    (void)sdl3vk_raise_window();
}
#endif

#ifdef _WIN32
static void sr_gui_raise_gdi_window(void *context) {
    HWND window = (HWND)context;
    if (!window) return;
    BringWindowToTop(window);
    if (!SetForegroundWindow(window)) {
        fprintf(stderr, "gui_init: Win32 refused foreground activation for the game window\n");
    }
}
#endif

static void sr_gui_request_launcher_foreground(
    bool window_visible,
    void *context,
    SrGuiForegroundRequestFn raise_window,
    const char *backend
) {
    if (sr_gui_request_launcher_foreground_once(
            getenv("SR_BOOT_EVENT_FILE"), window_visible,
            sr_gui_is_headless_presenter(), &s_foreground_request_issued,
            context, raise_window)) {
        sr_gui_boot_event("BOOT_EVENT phase=window_foreground_requested backend=%s",
                          backend);
    }
}
#endif

/* PSP button bits (sceCtrl), named in src/core/nk_input_profile.h: SELECT 0x1, START 0x8,
 * UP 0x10, RIGHT 0x20, DOWN 0x40, LEFT 0x80, LTRIG 0x100, RTRIG 0x200, TRIANGLE 0x1000,
 * CIRCLE 0x2000, CROSS 0x4000, SQUARE 0x8000. One table, shared with the scripted-input
 * route language (src/rt/hle.c), so a live press and a scripted press cannot disagree. */
#ifdef _WIN32
static uint32_t read_keys(void) {
    uint32_t b = 0;
    #define K(vk,bit) do { if (GetAsyncKeyState(vk) & 0x8000) b |= (bit); } while (0)
    K(VK_RETURN, NK_PSP_BTN_START_BIT);         /* Enter  -> START  */
    K(VK_RSHIFT, NK_PSP_BTN_SELECT_BIT); K(VK_LSHIFT, NK_PSP_BTN_SELECT_BIT); /* Shift -> SELECT */
    K('X', NK_PSP_BTN_CROSS_BIT);               /* X -> CROSS   (confirm) */
    K('Z', NK_PSP_BTN_CIRCLE_BIT);              /* Z -> CIRCLE  (back)    */
    K('A', NK_PSP_BTN_SQUARE_BIT);              /* A -> SQUARE  */
    K('S', NK_PSP_BTN_TRIANGLE_BIT);            /* S -> TRIANGLE */
    K('Q', NK_PSP_BTN_LTRIGGER_BIT);            /* Q -> L */
    K('W', NK_PSP_BTN_RTRIGGER_BIT);            /* W -> R */
    K(VK_UP, NK_PSP_BTN_UP_BIT); K(VK_DOWN, NK_PSP_BTN_DOWN_BIT);
    K(VK_LEFT, NK_PSP_BTN_LEFT_BIT); K(VK_RIGHT, NK_PSP_BTN_RIGHT_BIT);
    #undef K
    return b;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE || m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { PostQuitMessage(0); return 0; }
    return DefWindowProc(h, m, w, l);
}
#endif /* _WIN32: the keyboard fallback and the window class are Win32-only */

#if defined(SR_SDL3VK) || defined(_WIN32)
static void gui_release_presenter(void);
static void gui_exit_cleanup(void) {
    sr_perf_shutdown();
    gui_release_presenter();
}
#endif

void gui_init(const char *title) {
    (void)title;
#if defined(SR_SDL3VK) || defined(_WIN32)
    static int cleanup_registered;
    if (!cleanup_registered && atexit(gui_exit_cleanup) == 0) cleanup_registered = 1;
#endif
    gui_headless_acceptance_parse();
    if (s_headless_accept_requested && s_headless_accept_invalid) {
        sr_gui_boot_event("BOOT_EVENT phase=headless_acceptance status=FAIL reason=invalid_frame_target");
        return;
    }
    const char *video = getenv("SR_VIDEO");
    if (s_headless_accept_requested && video &&
        strcmp(video, "offscreen") == 0) {
        sr_gui_boot_event("BOOT_EVENT phase=headless_acceptance status=FAIL reason=offscreen_presenter");
        return;
    }
    const char *sdl_video_driver = getenv("SDL_VIDEODRIVER");
    const char *sdl_video_driver_alias = getenv("SDL_VIDEO_DRIVER");
    if (s_headless_accept_requested &&
        ((sdl_video_driver && strcmp(sdl_video_driver, "offscreen") == 0) ||
         (sdl_video_driver_alias && strcmp(sdl_video_driver_alias, "offscreen") == 0))) {
        sr_gui_boot_event("BOOT_EVENT phase=headless_acceptance status=FAIL reason=offscreen_sdl_driver");
        return;
    }
    if (video && strcmp(video, "offscreen") == 0) {
        /* The offscreen presenter is an explicit host-memory sink. It does not
         * initialize SDL video, create a window, or claim Vulkan presentation;
         * gui_present accepts a frame only after conversion has written this
         * allocated sink. Interactive routes keep their existing selection. */
        s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
        if (!s_px) {
            fprintf(stderr, "gui_init: offscreen presenter allocation failed\n");
            sr_gui_boot_event("BOOT_EVENT phase=window_unavailable backend=offscreen");
            return;
        }
        s_offscreen = 1;
        s_offscreen_frames = 0;
        s_last_ns = SDL_GetTicksNS();
        s_on = 1;
        /* window_ready is the established boot-gate name for the host-presenter
         * initialization milestone. The backend field makes the no-window case
         * explicit; this event does not claim that an OS window exists. */
        sr_gui_boot_event("BOOT_EVENT phase=window_ready backend=offscreen");
        return;
    }
#ifdef SR_SDL3VK
    /* SDL3+Vulkan presenter (src/rt/gpu_sdl3vk, Phase 0): default in this build;
     * SR_VIDEO=gdi falls back to the classic Win32/GDI window below. */
    {
        const char *v = getenv("SR_VIDEO");
        if (!v || strcmp(v, "gdi") != 0) {
            if (sdl3vk_init(title)) {
#ifdef SR_PUBLIC_SAFE
                /* Only the public audio backend exports this query; without a
                   callback the HUD reports audio status as unknown. */
                extern int sr_audio_is_active(void);
                sdl3vk_set_audio_active_cb(sr_audio_is_active);
#endif
                s_sdl3 = 1;
                s_px = (uint32_t *)malloc(PSP_W * PSP_H * 4);
                s_last_ns = SDL_GetTicksNS();
                s_on = 1;
                if (s_headless_accept_requested) s_headless_accept_ready = 1;
                /* The window draws the on-screen keyboard, so no native box is needed; a
                 * presenter that cannot draw keeps the box as the fallback (osk_text_entry.c). */
                sr_osk_overlay_set_host(osk_overlay_paint_available());
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
                sr_gui_request_launcher_foreground(true, NULL,
                                                   sr_gui_raise_sdl_window,
                                                   "vulkan");
                sr_gui_boot_event("BOOT_EVENT phase=window_ready backend=vulkan");
                return;
            }
#ifdef _WIN32
            fprintf(stderr, "gui_init: SDL3/Vulkan init failed; falling back to GDI\n");
#else
            fprintf(stderr, "gui_init: SDL3/Vulkan init failed\n");
#endif
        }
    }
#endif
#ifdef _WIN32
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.lpszClassName = "psp_recomp";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
    RECT r = {0, 0, PSP_W * 2, PSP_H * 2};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    /* Centred on the monitor that holds the launcher window, as the SDL presenter does
     * (SR_LAUNCHER_DISPLAY_BOUNDS, recomp.h); without a usable value Windows places it. */
    int win_x = CW_USEDEFAULT, win_y = CW_USEDEFAULT;
    {
        const char *value = getenv("SR_LAUNCHER_DISPLAY_BOUNDS");
        int b[4];
        if (value && value[0] && !sr_parse_display_bounds(value, b)) {
            fprintf(stderr, "gui_init: SR_LAUNCHER_DISPLAY_BOUNDS=%s is not x,y,w,h; "
                            "opening on the primary display\n", value);
        } else if (value && value[0]) {
            POINT centre = { b[0] + b[2] / 2, b[1] + b[3] / 2 };
            HMONITOR mon = MonitorFromPoint(centre, MONITOR_DEFAULTTONULL);
            MONITORINFO mi;
            mi.cbSize = sizeof(mi);
            if (!mon) {
                fprintf(stderr, "gui_init: no monitor contains the launcher's display %s; "
                                "opening on the primary display\n", value);
                mon = MonitorFromPoint(centre, MONITOR_DEFAULTTOPRIMARY);
            }
            if (GetMonitorInfoA(mon, &mi)) {
                win_x = mi.rcWork.left +
                        ((mi.rcWork.right - mi.rcWork.left) - (r.right - r.left)) / 2;
                win_y = mi.rcWork.top +
                        ((mi.rcWork.bottom - mi.rcWork.top) - (r.bottom - r.top)) / 2;
            }
        }
    }
    s_hwnd = CreateWindowA("psp_recomp", title ? title : SR_APP_TITLE,
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, win_x, win_y,
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
    if (s_headless_accept_requested && s_hwnd) s_headless_accept_ready = 1;
    sr_gui_request_launcher_foreground(
        s_hwnd && IsWindowVisible(s_hwnd), s_hwnd,
        sr_gui_raise_gdi_window, "gdi");
    sr_gui_boot_event("BOOT_EVENT phase=window_ready backend=gdi");
#else /* !_WIN32: no GDI presenter on this host */
    /* No Win32/GDI window exists here, so there is no fallback to take. The
     * refusal is reported and s_on stays 0: the runtime keeps executing the
     * guest and the framebuffer capture/diagnostics keep working, but nothing
     * claims a window that was never created. */
    fprintf(stderr,
            "gui_init: SDL3/Vulkan init failed and no Win32/GDI presenter exists on "
            "this host; running without a window (framebuffer capture still works)\n");
    s_on = 0;
    sr_gui_boot_event("BOOT_EVENT phase=window_unavailable backend=none");
#endif /* _WIN32 */
}

int gui_on(void) { return s_on; }
/* 1 only while a person is looking at a real host window: a presenter is up and it is not the
 * offscreen sink, a headless/dummy-video-driver host, or a headless acceptance launch. Those
 * unattended presenters also report gui_on(), but nobody there can press START, so the
 * runtime's bootstrap input (h_CtrlButtons) must keep running for them. */
int gui_interactive(void) {
    if (!s_on || s_offscreen || s_headless_accept_requested) return 0;
#if defined(SR_SDL3VK) || defined(_WIN32)
    if (sr_gui_is_headless_presenter()) return 0;
#endif
    return 1;
}
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

/* Gamepad presses since the last call (osk_overlay.h). The keyboard takes them every VBLANK,
 * so the list never builds up while it is closed. Returns 0 when no SDL3 presenter is up: the
 * GDI and offscreen presenters have no SDL3 gamepad. The overlay host governs drawing, not this
 * read; the presses are drained whether or not the host is on. */
uint32_t gui_pad_pulses_take(void) {
#ifdef SR_SDL3VK
    if (s_sdl3) return sdl3vk_take_pad_pulses();
#endif
    return 0u;
}

/* Present a framebuffer at guest address fbaddr. fmt: 0=5650, 1=5551, 2=4444, 3=8888.
 * stride is in pixels (PSP buffer width, typically 512). */
/* Convert the guest framebuffer to the BGRA words both presenters consume. The
 * offscreen sink converts too, so this is not behind a presenter #if. */
static void convert_fb_pixels(uint32_t fbaddr, int fmt, uint32_t stride) {
    for (int y = 0; y < PSP_H; y++) {
        for (int x = 0; x < PSP_W; x++) {
            uint32_t i = (uint32_t)(y * (int)stride + x);
            int rr, gg, bb;
            if (fmt == 3) {
                uint32_t p;
                /* display_host_span_valid checks the complete framebuffer before this
                 * conversion. Read framebuffer storage literally: sr_r32 reserves the
                 * GE status port at 0x04084000, which can otherwise replace a pixel when
                 * a PSP VRAM framebuffer spans that address. */
                memcpy(&p, SR_HOST(fbaddr + i * 4), sizeof p);
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
}

/* A guest frame: the conversion plus the first-frame boot milestone. */
static void convert_fb(uint32_t fbaddr, int fmt, uint32_t stride) {
    convert_fb_pixels(fbaddr, fmt, stride);
    static int s_first_frame = 1;
    if (s_first_frame) {
        s_first_frame = 0;
        unsigned long nz = 0;
        for (int yy = 0; yy < PSP_H; yy++)
            for (int xx = 0; xx < PSP_W; xx++)
                if (s_px[yy * PSP_W + xx] & 0x00FFFFFF) nz++;
        fprintf(stderr, "[FIRST_FRAME] non_zero_pixels=%lu / total=%u\n", nz, PSP_W * PSP_H);
        sr_gui_boot_event("BOOT_EVENT phase=first_frame source=cpu nonzero_pixels=%lu total_pixels=%u",
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

#if defined(SR_SDL3VK) || defined(_WIN32)
static void gui_window_closed_exit(void);   /* after the pause presenter, whose canvas it frees */
#endif

void gui_pump(void) {
    if (!s_on) return;
    if (s_offscreen) return;
#ifdef SR_SDL3VK
    if (s_sdl3) {
        /* A window close first notifies the guest's exit callback; the run goes on while the
         * guest has its bounded window to call sceKernelExitGame (hle.c, #1392). */
        int alive = sdl3vk_poll() ? !sr_exit_request_expired() : sr_exit_request_quit();
        sync_sdl_input();
        if (!alive) { gui_window_closed_exit(); }
        return;
    }
#endif
#ifdef _WIN32
    MSG msg;
    while (PeekMessageA(&msg, 0, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { gui_window_closed_exit(); }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
#endif
}

#ifdef _WIN32
/* Blit the converted frame (s_px) into the GDI window. 1 = the frame was drawn. */
static int gdi_blit(void) {
    if (!s_hwnd) return 0;
    HDC dc = GetDC(s_hwnd);
    if (!dc) return 0;
    RECT cr;
    if (!GetClientRect(s_hwnd, &cr)) {
        ReleaseDC(s_hwnd, dc);
        return 0;
    }
    int scanlines = StretchDIBits(dc, 0, 0, cr.right, cr.bottom, 0, 0, PSP_W, PSP_H,
                                  s_px, &s_bmi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(s_hwnd, dc);
    return scanlines > 0;
}
#endif

/* CPU-backed presenters consume guest memory, so cross the same lazy GE read
 * barrier as the scalar accessors before converting a complete framebuffer. The
 * direct Vulkan present path bypasses this helper and keeps its target resident. */
static int display_cpu_source_current(uint32_t fbaddr, int fmt, uint32_t stride) {
    if (fmt < 0 || fmt > 3 || stride == 0u) return 0;
    uint32_t row_bytes, total_bytes;
    uint32_t bytes_per_pixel = fmt == 3 ? 4u : 2u;
    if (!sr_size_mul_ok(stride, bytes_per_pixel, &row_bytes) ||
        !sr_size_mul_ok(row_bytes, PSP_H, &total_bytes)) return 0;
    return sr_guest_span_readable_current(fbaddr, total_bytes);
}

int gui_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    if (!s_on) return 0;
    if (stride == 0) stride = 512;

    if (!present_slot_due()) {
        /* No presenter runs, so an armed capture stays unserviced; the caller resolves it
         * as "nothing attempted" when this returns (fbcap.h). */
        if (sr_perf_enabled) sr_perf_present_skip();
        gui_pump();
        return 0;
    }

    if (s_offscreen) {
        /* display_present_active validates the complete guest span before this
         * call. Conversion is the sink operation: the accepted frame is now
         * resident in the allocated host buffer, with no window or Vulkan WSI
         * dependency. */
        if (!display_cpu_source_current(fbaddr, fmt, stride)) {
            sr_capture_fail("guest framebuffer is unavailable for CPU presentation");
            return 0;
        }
        convert_fb(fbaddr, fmt, stride);
        s_offscreen_frames++;
        if (getenv("SR_PRESENT_TRACE")) {
            sr_gui_boot_event("BOOT_EVENT phase=frame_present backend=offscreen frame=%llu",
                    (unsigned long long)s_offscreen_frames);
        }
        /* The converted frame is what this sink accepted: an armed capture publishes
         * exactly these pixels, the same host frame the other presenters show. */
        sr_capture_serve_host(s_px, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX);
        return 1;
    }

    int accepted = 1;
#ifdef SR_SDL3VK
    if (s_sdl3) {
        /* 1. GPU VRAM image fast path: any frame the GPU rasterizer produced is presented
         *    directly from its Vulkan image. Returns 1 on success (skip GDI), -1 when the
         *    address isn't GPU-resident (CPU movie frames etc.) — fall through to BGRA. */
        accepted = 0;
        /* While the on-screen keyboard is open the frame must pass through the host copy the
         * keyboard is drawn on, so the GPU fast path steps aside (-1: CPU path below). The
         * decision is taken once: osk_open also gates the guest-VRAM sync before that copy. */
        int osk_open = sr_osk_overlay_active();
        int res = osk_open ? -1 : gegpu_present(fbaddr, fmt, stride);
        sync_sdl_input();
        if (res == 0) { gui_window_closed_exit(); }
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
                sr_gui_boot_event("BOOT_EVENT phase=first_frame source=gpu presented=1");
            }
            accepted = 1;
            goto pace;
        }

        /* 2. CPU-rendered fallback: convert guest VRAM to BGRA and blit through SDL3/Vulkan. */
        if (osk_open) {
            /* The GPU fast path was skipped for this frame, so a frame the rasterizer produced
             * may exist only in its Vulkan image. Materialize that target into guest VRAM before
             * convert_fb reads it, the boundary hle.c snapshot_sync_ok() enforces before a dump.
             * NO_TARGET: the frame was never GPU-resident and guest VRAM is already current.
             * FAILED: decline the frame rather than draw the keyboard over stale pixels. */
            GeGpuFbDescriptor d = { fbaddr, (uint32_t)fmt, stride, PSP_W, PSP_H };
            if (gegpu_sync_guest_fb(&d) == GEGPU_SYNC_FAILED) {
                static int s_osk_sync_warned = 0;
                if (s_osk_sync_warned < 3) {
                    s_osk_sync_warned++;
                    fprintf(stderr, "OSK: guest framebuffer synchronisation FAILED for addr=0x%08x "
                                    "fmt=%d stride=%u -- frame not presented\n", fbaddr, fmt, stride);
                }
                sr_capture_fail("keyboard guest framebuffer sync failed");
                goto pace;
            }
        }
        if (res == GEGPU_PRESENT_TARGET_FAILED) {
            /* gegpu_present() found a GPU-valid target for this frame, but preparing or presenting
             * it failed (issue #827). Its newest pixels may exist only in that Vulkan image, so
             * guest VRAM is stale. Materialize it with the same sync the keyboard path uses before
             * the fall-through below reads it. FAILED declines the frame instead of presenting the
             * older pixels still in guest VRAM. */
            GeGpuFbDescriptor d = { fbaddr, (uint32_t)fmt, stride, PSP_W, PSP_H };
            if (gegpu_sync_guest_fb(&d) == GEGPU_SYNC_FAILED) {
                static int s_target_sync_warned = 0;
                if (s_target_sync_warned < 3) {
                    s_target_sync_warned++;
                    fprintf(stderr, "GPU present: target preparation or presentation FAILED and guest "
                                    "framebuffer synchronisation FAILED for addr=0x%08x fmt=%d "
                                    "stride=%u -- frame not presented\n", fbaddr, fmt, stride);
                }
                sr_capture_fail("GPU present target failed: guest framebuffer sync failed");
                goto pace;
            }
        }
        if (!display_cpu_source_current(fbaddr, fmt, stride)) {
            sr_capture_fail("guest framebuffer is unavailable for CPU presentation");
            goto pace;
        }
        convert_fb(fbaddr, fmt, stride);
        /* The keyboard is drawn on the host copy only: the same pixels the window and the
         * frame capture receive, and guest VRAM is untouched. */
        if (sr_osk_overlay_active()) osk_overlay_paint(sr_osk_overlay_view(), s_px, PSP_W, PSP_H);
        int present_result = sdl3vk_present_rgba(s_px);
        sync_sdl_input();
        if (present_result == 0) { gui_window_closed_exit(); }
        accepted = present_result == 1;
        goto pace;
    }
#endif

#ifdef _WIN32
    /* GDI fallback is keyboard-only; controllers are handled by the SDL3 gamepad
     * subsystem (src/rt/gpu_sdl3vk) on the default Vulkan path. */
    gui_pump();
    s_buttons = read_keys();
    s_lx = 128; s_ly = 128;
    s_pad_present = 0;

    if (!display_cpu_source_current(fbaddr, fmt, stride)) {
        sr_capture_fail("guest framebuffer is unavailable for CPU presentation");
        return 0;
    }
    convert_fb(fbaddr, fmt, stride);
    if (!gdi_blit()) {
        sr_capture_fail("GDI present failed");
        return 0;
    }
    /* The DIB just blitted is the converted frame: an armed capture publishes it. */
    sr_capture_serve_host(s_px, PSP_W, PSP_H, PSP_W * 4u, SR_CAP_ORDER_BGRX);
#else
    /* No GDI presenter on this host and the SDL3 presenter is not active: gui_init
     * reported that, so there is nothing to present and nothing to claim. */
    return 0;
#endif

#ifdef SR_SDL3VK
pace:
#endif
    if (accepted) {
        s_shown_fb.addr = fbaddr;
        s_shown_fb.fmt = fmt;
        s_shown_fb.stride = stride;
        s_shown_fb.valid = 1;
    }
    gui_headless_acceptance_record_present(accepted);
    /* Pace to ~60 Hz so the intro/menus play at a watchable speed. Skipped when the scheduler
     * already paces vblanks to real time (the default): pacing twice on unaligned grids makes
     * frames miss their vblank and costs a whole extra period (30/20 fps quantization). */
    {
        extern int sched_vbl_paced(void);
        if (sched_vbl_paced()) { s_last_ns = SDL_GetTicksNS(); return accepted; }
    }
    uint64_t now_ns = SDL_GetTicksNS();
    const uint64_t period_ns = 1000000000u / 60u;
    if (now_ns - s_last_ns < period_ns)
        SDL_DelayPrecise(period_ns - (now_ns - s_last_ns));
    s_last_ns = SDL_GetTicksNS();
    return accepted;
}

/* ---- in-game pause presenter (#1159) -------------------------------------------------
 * The scheduler stops the game at a switch point and calls these from its pause loop
 * (SrPauseHost, sched.c). While paused the window shows the frame the guest last had
 * displayed under the overlay scrim, redrawn at the display rate, and only waits on events
 * while minimised. The overlay owns all input: the game reads a released pad, and a button
 * held when the overlay closes stays hidden from the game until it is let go (sdl3vk.h).
 * These repeat presents are not guest presents: they never reach the frame capture, the
 * headless acceptance count, the first-frame milestone or the guest frame rate. */
#ifdef SR_SDL3VK
static int          s_pause_open;
static int          s_pause_resume;      /* the overlay asked to resume */
static int          s_pause_dirty;       /* the overlay image must be drawn again */
static int          s_pause_base_dirty;  /* the frozen frame under the scrim must be composed again */
static int          s_pause_have_frame;  /* s_px holds the frozen game frame */
static int          s_pause_failure_reported;
static uint64_t     s_pause_next_ns;
static SDL_Surface  *s_pause_base;       /* the frozen frame under the scrim, at the window's size */
static SDL_Surface  *s_pause_surface;    /* the overlay image: the base with the pages over it */
static SDL_Renderer *s_pause_ren;        /* draws the pages into s_pause_surface */
static NkOverlayHost *s_pause_host;      /* the pages; NULL shows the base alone (no SDL3_ttf or fonts) */
static int          s_pause_host_tried;
static NkOverlayControls s_pause_controls;
/* The developer pages (#314), filled only in developer mode. */
static NkOverlayVram s_pause_vram;
static NkUiImage     s_pause_vram_image;
static uint32_t     *s_pause_vram_px;    /* the decoded window, GE_VRAM_VIEW_MAX_* words */
static NkOverlayProfile s_pause_profile;
static SrPerfLiveRow s_pause_profile_rows[SR_PERF_LIVE_ROWS];
static char          s_pause_profile_top[2][64];

int sr_savedata_pause_busy(void);   /* hle.c: a save is in progress, so quitting is refused */

static void gui_pause_free_canvas(void) {
    nk_overlay_host_unbind(s_pause_host);
    if (s_pause_ren) SDL_DestroyRenderer(s_pause_ren);
    if (s_pause_surface) SDL_DestroySurface(s_pause_surface);
    if (s_pause_base) SDL_DestroySurface(s_pause_base);
    s_pause_ren = NULL;
    s_pause_surface = NULL;
    s_pause_base = NULL;
}

/* The frame under the overlay is the displayed framebuffer as it is now. A framebuffer the
 * GPU rasterizer drew is brought back to guest memory first, the coherence step the keyboard
 * takes before reading it; nothing the guest observes changes. */
static int gui_pause_freeze(void) {
    if (!s_shown_fb.valid) return 0;
    GeGpuFbDescriptor d = { s_shown_fb.addr, (uint32_t)s_shown_fb.fmt, s_shown_fb.stride, PSP_W, PSP_H };
    if (gegpu_sync_guest_fb(&d) == GEGPU_SYNC_FAILED) return 0;
    if (!display_cpu_source_current(s_shown_fb.addr, s_shown_fb.fmt, s_shown_fb.stride)) return 0;
    convert_fb_pixels(s_shown_fb.addr, s_shown_fb.fmt, s_shown_fb.stride);
    return 1;
}

/* Composes the base at the swapchain's size: the frozen frame placed where a game frame
 * would be (with the scaling in force), then the scrim over all of it. */
static int gui_pause_compose_base(int w, int h, SDL_PixelFormat format) {
    if (!s_pause_base || s_pause_base->w != w || s_pause_base->h != h || s_pause_base->format != format) {
        if (s_pause_base) SDL_DestroySurface(s_pause_base);
        s_pause_base = SDL_CreateSurface(w, h, format);
        if (!s_pause_base) return 0;
    }
    SDL_Renderer *ren = SDL_CreateSoftwareRenderer(s_pause_base);
    if (!ren) return 0;
    int ok = 1;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (s_pause_have_frame) {
        SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STATIC,
                                             PSP_W, PSP_H);
        if (!tex || !SDL_UpdateTexture(tex, NULL, s_px, PSP_W * 4)) {
            ok = 0;
        } else {
            Sdl3VkPresentPlan plan = sdl3vk_present_plan(sdl3vk_present_scaling(), PSP_W, PSP_H, w, h, INT32_MAX);
            if (plan.w > 0 && plan.h > 0) {
                SDL_FRect dst = { (float)plan.x, (float)plan.y, (float)plan.w, (float)plan.h };
                SDL_SetTextureScaleMode(tex, plan.nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
                SDL_RenderTexture(ren, tex, NULL, &dst);
            }
        }
        if (tex) SDL_DestroyTexture(tex);
    }
    const uint32_t scrim = nk_ui_theme_n1()->scrim;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, (Uint8)(scrim >> 24), (Uint8)(scrim >> 16),
                           (Uint8)(scrim >> 8), (Uint8)scrim);
    SDL_RenderFillRect(ren, NULL);
    SDL_RenderPresent(ren);
    SDL_DestroyRenderer(ren);
    return ok;
}

static int gui_pause_scaling_to_overlay(int mode) {
    switch (mode) {
    case SDL3VK_SCALING_INTEGER:  return NK_OVERLAY_SCALING_INTEGER;
    case SDL3VK_SCALING_BILINEAR: return NK_OVERLAY_SCALING_SMOOTH;
    default:                      return NK_OVERLAY_SCALING_SHARP;
    }
}

static int gui_pause_scaling_from_overlay(int scaling) {
    switch (scaling) {
    case NK_OVERLAY_SCALING_INTEGER: return SDL3VK_SCALING_INTEGER;
    case NK_OVERLAY_SCALING_SMOOTH:  return SDL3VK_SCALING_BILINEAR;
    default:                         return SDL3VK_SCALING_SHARP;
    }
}

/* The controls page describes the mapping in force: the session's input profile, its keys
 * and pad bindings, with the pad's buttons named for the pad that is there. */
static void gui_pause_describe_controls(void) {
    nk_overlay_describe_controls(&s_pause_controls, sdl3vk_get_input_profile(),
                                 nk_overlay_host_pad_family(s_pause_host));
}

/* A GE-local framebuffer offset, or any view of VRAM, as its 0x04000000 address, the fold
 * ge_framebuffer() applies to the draw target; any other address is left as it is. */
static uint32_t gui_vram_fold(uint32_t addr) {
    if (addr < 0x00200000u || (addr & 0x0F000000u) == 0x04000000u)
        return 0x04000000u | (addr & 0x001FFFFFu);
    return addr;
}

/* The surfaces the GE state names while the game is paused, as the VRAMDUMP sidecar names
 * them: the displayed framebuffer, the draw target, the depth buffer and the bound texture. */
static void gui_pause_describe_vram(void) {
    const GeState *ge = ge_state_ptr();
    NkOverlayVram *v = &s_pause_vram;
    memset(v->sources, 0, sizeof(v->sources));
    memset(v->present, 0, sizeof(v->present));
    if (s_shown_fb.valid) {
        NkOverlayVramSurface s = { s_shown_fb.addr, s_shown_fb.stride, PSP_W, PSP_H,
                                   (uint32_t)s_shown_fb.fmt & 3u };
        v->sources[NK_OVERLAY_VRAM_DISPLAY] = s;
        v->present[NK_OVERLAY_VRAM_DISPLAY] = 1;
    }
    if (!ge) return;
    if (ge->fbw) {
        NkOverlayVramSurface s = { ge_framebuffer(), ge->fbw, PSP_W, PSP_H, ge->fbfmt & 3u };
        v->sources[NK_OVERLAY_VRAM_DRAW] = s;
        v->present[NK_OVERLAY_VRAM_DRAW] = 1;
    }
    if (ge->zbp) {
        NkOverlayVramSurface s = { gui_vram_fold(ge->zbp), ge->zbw ? ge->zbw : (ge->fbw ? ge->fbw : 512u),
                                   PSP_W, PSP_H, NK_OVERLAY_VRAM_DEPTH16 };
        v->sources[NK_OVERLAY_VRAM_DEPTH] = s;
        v->present[NK_OVERLAY_VRAM_DEPTH] = 1;
    }
    if (ge->tex_addr && ge->tex_w > 0 && ge->tex_h > 0) {
        NkOverlayVramSurface s = { ge->tex_addr, ge->tex_bufw ? ge->tex_bufw : (uint32_t)ge->tex_w,
                                   (uint32_t)ge->tex_w, (uint32_t)ge->tex_h, ge->tex_fmt };
        v->sources[NK_OVERLAY_VRAM_TEXTURE] = s;
        v->present[NK_OVERLAY_VRAM_TEXTURE] = 1;
        v->texture_swizzled = ge->tex_swizzle != 0;
    }
}

/* Decodes the viewer's window into the page's image. Reads only: the surface decoder copies
 * guest bytes as stored, and the bound texture goes through the texture decoder with the GE
 * state left as it is (ge_shared.h). Alpha is shown opaque, as the PSP display shows a
 * framebuffer. */
static void gui_pause_decode_vram(void) {
    const NkOverlayVramView *view = nk_overlay_host_vram_view(s_pause_host);
    const size_t capacity = (size_t)GE_VRAM_VIEW_MAX_WIDTH * GE_VRAM_VIEW_MAX_HEIGHT;
    uint32_t width, height;
    int ok;
    s_pause_vram.image = NULL;
    s_pause_vram.note = NULL;
    if (!view || !view->chosen) return;
    if (!s_pause_vram_px) s_pause_vram_px = (uint32_t *)malloc(capacity * sizeof(uint32_t));
    if (!s_pause_vram_px) {
        s_pause_vram.note = "Out of memory for the decoded window.";
        return;
    }
    if (view->source == NK_OVERLAY_VRAM_TEXTURE) {
        const GeState *ge = ge_state_ptr();
        ok = ge && ge_vram_decode_bound_texture(s_pause_vram_px, capacity);
        width = ok ? (uint32_t)ge->tex_w : 0u;
        height = ok ? (uint32_t)ge->tex_h : 0u;
        if (!ok)
            s_pause_vram.note = "The bound texture cannot be read now: it is outside guest memory, "
                                "larger than 512 pixels, or a GE capture is running.";
    } else {
        const NkOverlayVramSurface *w = &view->view;
        ok = ge_vram_decode_surface(w->addr, w->width, w->height, w->stride, w->format,
                                    s_pause_vram_px);
        width = w->width;
        height = w->height;
        if (!ok) s_pause_vram.note = "This window cannot be read now.";
    }
    if (!ok) return;
    for (size_t i = 0; i < (size_t)width * height; i++) s_pause_vram_px[i] |= 0xFF000000u;
    s_pause_vram_image.width = width;
    s_pause_vram_image.height = height;
    s_pause_vram_image.rgba_pm = (uint8_t *)s_pause_vram_px;
    s_pause_vram_image.serial = nk_ui_image_new_serial();
    s_pause_vram.image = &s_pause_vram_image;
}

/* The Profile page's rows and bars from the nakagawa-perf-v1 counters (perf.h). Opening the
 * page turns the counters on when nothing else had: they collect from then on. */
static void gui_pause_describe_profile(void) {
    NkOverlayProfile *p = &s_pause_profile;
    const SrPerfHistory *history = sr_perf_history();
    int was_on = sr_perf_enabled;
    unsigned rows;
    size_t bars;
    memset(p, 0, sizeof(*p));
    if (!was_on) sr_perf_enable_counters();
    rows = sr_perf_live_rows(sr_perf_history_at(history, 0), s_pause_profile_rows,
                             SR_PERF_LIVE_ROWS);
    for (unsigned i = 0; i < rows; i++) {
        p->rows[p->row_count].label = s_pause_profile_rows[i].label;
        p->rows[p->row_count].value = s_pause_profile_rows[i].value;
        p->row_count++;
    }
    for (unsigned direction = 0; rows && direction < 2u; direction++) {
        SrPerfTopPc top;
        if (!sr_perf_top_transitions(direction, &top, 1)) continue;
        snprintf(s_pause_profile_top[direction], sizeof(s_pause_profile_top[direction]),
                 "0x%08X x%llu %s", (unsigned)top.pc, (unsigned long long)top.count, top.reason);
        p->rows[p->row_count].label = direction ? "Back to AOT" : "To interpreter";
        p->rows[p->row_count].value = s_pause_profile_top[direction];
        p->row_count++;
    }
    bars = history->count < NK_OVERLAY_PROFILE_BARS ? history->count : NK_OVERLAY_PROFILE_BARS;
    for (size_t i = 0; i < bars; i++) {
        const SrPerfLiveInterval *iv = sr_perf_history_at(history, (unsigned)(bars - 1u - i));
        double wall = iv && iv->wall_ns ? (double)iv->wall_ns : 0.0;
        if (!wall) continue;
        p->bars[p->bar_count][NK_OVERLAY_PROFILE_CPU] = (float)((double)iv->cpu_ns / wall);
        p->bars[p->bar_count][NK_OVERLAY_PROFILE_GE_WAIT] = (float)((double)iv->ge_wait_ns / wall);
        p->bars[p->bar_count][NK_OVERLAY_PROFILE_PRESENT] = (float)((double)iv->present_ns / wall);
        p->bars[p->bar_count][NK_OVERLAY_PROFILE_IDLE] = (float)((double)iv->idle_ns / wall);
        p->bar_count++;
    }
    p->note = rows ? "The last closed second; the transition PCs cover the whole run. In "
                     "developer mode F3 shows these rows live."
          : was_on ? "Resume for a second: the counters close an interval each second."
                   : "The counters start now. Resume the game, then pause again.";
}

/* Carries out what the person chose on a page. */
static void gui_pause_act(NkOverlayAction action) {
    switch (action.kind) {
    case NK_OVERLAY_ACT_RESUME:
        s_pause_resume = 1;
        break;
    case NK_OVERLAY_ACT_QUIT:
        /* Confirmed, and refused by the page while a save is in progress: the run ends the
         * way a window close ends it, and the player is back in the library. */
        fprintf(stderr, "SR_PAUSE: quit to library\n");
        fflush(stderr);
        gui_window_closed_exit();
        break;
    case NK_OVERLAY_ACT_SET_SCALING:
        if (sdl3vk_set_present_scaling(gui_pause_scaling_from_overlay(action.value)))
            s_pause_base_dirty = 1;
        s_pause_dirty = 1;
        break;
    case NK_OVERLAY_ACT_SET_HUD:
        sdl3vk_hud_set_enabled(action.value != 0);
        s_pause_dirty = 1;
        break;
    case NK_OVERLAY_ACT_VRAM_VIEW:
        if (sdl3vk_developer_mode()) gui_pause_decode_vram();
        s_pause_dirty = 1;
        break;
    default:
        break;
    }
}

/* Draws the overlay image at the swapchain's size: the base, then the pages over it. */
static int gui_pause_compose(int w, int h, int rgba) {
    SDL_PixelFormat format = rgba ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_BGRA32;
    if (!s_pause_surface || s_pause_surface->w != w || s_pause_surface->h != h ||
        s_pause_surface->format != format) {
        gui_pause_free_canvas();
        s_pause_surface = SDL_CreateSurface(w, h, format);
        s_pause_ren = s_pause_surface ? SDL_CreateSoftwareRenderer(s_pause_surface) : NULL;
        if (!s_pause_ren) {
            gui_pause_free_canvas();
            return 0;
        }
        s_pause_base_dirty = 1;
    }
    if (s_pause_base_dirty) {
        if (!gui_pause_compose_base(w, h, format)) return 0;
        s_pause_base_dirty = 0;
    }
    SDL_SetSurfaceBlendMode(s_pause_base, SDL_BLENDMODE_NONE);
    if (!SDL_BlitSurface(s_pause_base, NULL, s_pause_surface, NULL)) return 0;
    if (s_pause_host) {
        NkOverlayModel model;
        NkOverlayAction action;
        char game_title[SR_WINDOW_TITLE_GAME_MAX + 1];
        float display_scale = 1.0f, pixels_per_point = 1.0f;
        (void)sdl3vk_window_scales(&display_scale, &pixels_per_point);
        gui_pause_describe_controls();
        memset(&model, 0, sizeof(model));
        /* The launcher's SR_GAME_TITLE, cleaned as the window caption cleans it. */
        (void)sr_window_title_sanitize(getenv("SR_GAME_TITLE"), game_title, sizeof(game_title));
        model.title_name = game_title;
        model.disc_id = getenv("SR_DISC_ID");
        model.scaling = gui_pause_scaling_to_overlay(sdl3vk_present_scaling());
        model.hud_on = sdl3vk_hud_is_enabled() != 0;
        model.saving = sr_savedata_pause_busy() != 0;
        model.controls = s_pause_controls.rows;
        model.control_count = s_pause_controls.count;
        /* The developer pages exist only in developer mode (#314); the model carries them
         * nothing otherwise, and the pages refuse to draw without the flag. */
        model.developer = sdl3vk_developer_mode() != 0;
        if (model.developer) {
            gui_pause_describe_vram();
            model.vram = &s_pause_vram;
            if (nk_overlay_host_screen(s_pause_host) == NK_OVERLAY_SCREEN_PROFILE) {
                gui_pause_describe_profile();
                model.profile = &s_pause_profile;
            }
        }
        if (!nk_overlay_host_draw(s_pause_host, s_pause_ren, display_scale, pixels_per_point,
                                  &model, &action))
            return 0;
        SDL_RenderPresent(s_pause_ren);
        gui_pause_act(action);
    }
    return 1;
}

/* Every input and window event while the overlay is open (sdl3vk_set_input_claim). Guide
 * closes it from any page. The pages take the rest: Esc and the pad's back button go back a
 * page (and resume from the menu), F1 and the pad's Start resume. Without the pages, Esc,
 * F1 and the pad's back button (east) close it. */
static void gui_pause_input(const void *event, void *user) {
    const SDL_Event *ev = (const SDL_Event *)event;
    (void)user;
    switch (ev->type) {
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (ev->gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE) {
            if (ev->gbutton.down) s_pause_resume = 1;
            return;
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        s_pause_dirty = 1;
        s_pause_base_dirty = 1;
        s_pause_next_ns = 0;
        break;
    case SDL_EVENT_WINDOW_EXPOSED:
        s_pause_next_ns = 0;
        break;
    default:
        break;
    }
    if (nk_overlay_host_is_open(s_pause_host)) {
        if (nk_overlay_host_event(s_pause_host, ev)) {
            s_pause_dirty = 1;
            s_pause_next_ns = 0;
        }
        return;
    }
    switch (ev->type) {
    case SDL_EVENT_KEY_DOWN:
        if (!ev->key.repeat && (ev->key.key == SDLK_ESCAPE || ev->key.key == SDLK_F1))
            s_pause_resume = 1;
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (ev->gbutton.button == SDL_GAMEPAD_BUTTON_EAST)
            s_pause_resume = 1;
        break;
    default:
        break;
    }
}
#endif

int gui_pause_presenter(void) {
#ifdef SR_SDL3VK
    return s_sdl3;
#else
    return 0;
#endif
}

int gui_pause_busy_reason(void) {
    if (sr_osk_overlay_active()) return SR_PAUSE_REFUSED_KEYBOARD;
#ifdef SR_SDL3VK
    if (s_sdl3 && sdl3vk_renderer_terminal()) return SR_PAUSE_REFUSED_STOPPING;
#endif
    return 0;
}

void gui_pause_enter(void) {
#ifdef SR_SDL3VK
    if (!s_sdl3 || s_pause_open) return;
    s_pause_open = 1;
    s_pause_resume = 0;
    s_pause_dirty = 1;
    s_pause_base_dirty = 1;
    s_pause_next_ns = 0;
    sdl3vk_set_input_claim(gui_pause_input, NULL);
    s_buttons = 0;
    s_lx = 128;
    s_ly = 128;
    s_pause_have_frame = gui_pause_freeze();
    /* A new pause starts the viewer afresh (nk_overlay_open): nothing decoded is kept. */
    s_pause_vram.image = NULL;
    s_pause_vram.note = NULL;
    /* The pages load once, on the first pause: SDL3_ttf and the fonts come from the player's
     * folder. Without them the pause shows the frozen frame alone, and says why once. */
    if (!s_pause_host && !s_pause_host_tried) {
        char error[256];
        s_pause_host_tried = 1;
        s_pause_host = nk_overlay_host_create(getenv("SR_UI_ROOT"), error, sizeof(error));
        if (!s_pause_host)
            fprintf(stderr, "SR_PAUSE: the overlay pages are unavailable (%s); the pause shows the "
                            "frozen frame only\n", error);
    }
    nk_overlay_host_open(s_pause_host, sched_pause_source() == SR_PAUSE_SRC_PAD_LOST
                                           ? NK_OVERLAY_OPEN_PAD_LOST : NK_OVERLAY_OPEN_USER);
#endif
}

SrPauseFrame gui_pause_frame(void) {
#ifdef SR_SDL3VK
    if (!s_sdl3 || !s_pause_open) return SR_PAUSE_FRAME_RESUME;
    uint64_t now = SDL_GetTicksNS();
    uint32_t wait_ms = 100u;   /* minimised: wait on events, waking for the stop hook */
    if (!sdl3vk_window_minimized())
        wait_ms = s_pause_next_ns > now ? (uint32_t)((s_pause_next_ns - now + 999999u) / 1000000u) : 0u;
    if (wait_ms) sdl3vk_wait_events(wait_ms);
    if (!sdl3vk_poll()) { gui_window_closed_exit(); }
    if (s_pause_resume) return SR_PAUSE_FRAME_RESUME;
    now = SDL_GetTicksNS();
    if (sdl3vk_window_minimized() || now < s_pause_next_ns) return SR_PAUSE_FRAME_CONTINUE;
    uint64_t period = sdl3vk_display_period_ns();
    s_pause_next_ns = now + (period ? period : PSP_SCANOUT_PERIOD_NS);
    /* A held direction repeats, and a new page's second frame navigates by its layout. */
    if (nk_overlay_host_due(s_pause_host, (double)now / 1e9)) s_pause_dirty = 1;

    int w = 0, h = 0, rgba = 0;
    if (!sdl3vk_overlay_extent(&w, &h, &rgba)) return SR_PAUSE_FRAME_CONTINUE;
    const void *px = NULL;
    int pitch = 0;
    if (s_pause_dirty) {
        s_pause_dirty = 0;
        if (!gui_pause_compose(w, h, rgba)) {
            if (!s_pause_failure_reported) {
                s_pause_failure_reported = 1;
                fprintf(stderr, "SR_PAUSE: the overlay image (%dx%d) could not be drawn: %s\n",
                        w, h, SDL_GetError());
            }
            s_pause_dirty = 1;
            return SR_PAUSE_FRAME_CONTINUE;
        }
        if (s_pause_resume) return SR_PAUSE_FRAME_RESUME;
        px = s_pause_surface->pixels;
        pitch = s_pause_surface->pitch;
    }
    int presented = sdl3vk_present_overlay(px, pitch);
    if (presented == 0) { gui_window_closed_exit(); }
    if (presented == 2) {
        s_pause_dirty = 1;
        s_pause_base_dirty = 1;
        s_pause_next_ns = 0;
    } else if (presented < 0) {
        /* A renderer that can no longer present cannot show the overlay; the game goes on
         * and meets the same terminal renderer it would have met without a pause. */
        if (sdl3vk_renderer_terminal()) return SR_PAUSE_FRAME_RESUME;
        s_pause_dirty = 1;
    }
    return SR_PAUSE_FRAME_CONTINUE;
#else
    return SR_PAUSE_FRAME_RESUME;
#endif
}

void gui_pause_leave(void) {
#ifdef SR_SDL3VK
    if (!s_sdl3 || !s_pause_open) return;
    /* Events queued since the last frame are still the overlay's; the held state they leave
     * is what the release mask hides. A window close among them still ends the run. */
    if (!sdl3vk_poll()) { gui_window_closed_exit(); }
    sdl3vk_set_input_claim(NULL, NULL);
    sdl3vk_overlay_release();
    nk_overlay_host_close(s_pause_host);
    gui_pause_free_canvas();
    s_pause_open = 0;
    s_pause_resume = 0;
    s_buttons = sdl3vk_buttons();
    sdl3vk_analog(&s_lx, &s_ly);
#endif
}

#if defined(SR_SDL3VK) || defined(_WIN32)
/* Releases the presenter in reverse of gui_init: the pause canvas, the GPU GE (its objects live
 * on the presenter's device), the SDL3/Vulkan presenter, then the GDI window and the frame copy.
 * Each step clears what it released, so a second call does nothing. */
static void gui_release_presenter(void) {
#ifdef SR_SDL3VK
    if (s_sdl3) {
        if (s_pause_open) sdl3vk_set_input_claim(NULL, NULL);
        s_pause_open = 0;
        gui_pause_free_canvas();
        s_pause_vram.image = NULL;
        free(s_pause_vram_px);
        s_pause_vram_px = NULL;
        gegpu_shutdown();
        sdl3vk_shutdown();
        s_sdl3 = 0;
    }
#endif
#ifdef _WIN32
    if (s_hwnd) DestroyWindow(s_hwnd);
    s_hwnd = NULL;
#endif
    free(s_px);
    s_px = NULL;
    s_on = 0;
}

/* The user closed the window. The run ends through exit(), not _Exit: exit() flushes and closes
 * every stdio stream and runs the atexit handlers (the audio device and SR_AUDIODUMP close, the
 * flight-recorder bundle, the HLE, compatibility, profiling and stack-census reports), which
 * _Exit skipped. The guest threads are coroutines suspended on this OS thread (sr_coro.h) and are
 * not unwound; no atexit handler touches a guest thread, its stack or the scheduler, and the
 * runtime has no C++ static destructors. No cross-thread handshake is needed: gui_pump,
 * gui_present and the pause overlay's frame and leave hooks (called from sched.c) run on this
 * thread, which owns the perf counters, the CSV handle and the presenter. The SR_PERF summary is
 * written first, while the GPU GE it samples still exists; the presenter is then released here,
 * so no Vulkan device outlives exit() into the driver's own process teardown (#1072, #1377). */
static void gui_window_closed_exit(void) {
    sr_perf_shutdown();
    gui_release_presenter();
    exit(0);
}
#endif
#endif /* !SR_GUI_PRESENT_SELFTEST */

#ifdef SR_GUI_PRESENT_SELFTEST
/* A gate, not a debug aid: keep the assertions under release CFLAGS. */
#undef NDEBUG
#include <assert.h>

/* Synthetic host clock for the slot rule: an arbitrary non-zero origin, PSP scanouts one
 * period apart, and a submission offset inside its scanout. */
#define SELFTEST_T0_NS 1000000000000ull
#define SELFTEST_MS_NS 1000000ull

static void selftest_reset_slots(const char *cap) {
    s_present_cap = present_cap_from_value(cap);
    s_present_next_ns = 0;
}

/* Small deterministic generator (the PCG32 output step over an LCG state) for jitter
 * that is not a repeating pattern. */
static uint32_t selftest_next_random(uint64_t *state) {
    uint64_t old = *state;
    *state = old * 6364136223846793005ull + 1442695040888963407ull;
    uint32_t xorshifted = (uint32_t)(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = (uint32_t)(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

/* One flip per PSP scanout reaching the host with scheduling jitter. The first flip sets
 * the slot grid's phase; it arrives 3 ms late, so most later flips come earlier in their
 * scanout than the grid. Each is the only flip in its scanout and must be presented. The
 * floor rule (present only once the slot has started) dropped every flip that came ahead
 * of the first one's phase. */
static void selftest_jittered_scanout_flips_are_all_presented(void) {
    static const int32_t pattern_ms[] = { 3, -2, 1, -3, 0, 2, -1, -3, 3, -2 };
    const uint64_t period = PSP_SCANOUT_PERIOD_NS;
    selftest_reset_slots(NULL);
    unsigned presented = 0;
    for (unsigned k = 0; k < 600u; k++) {
        int64_t jitter = (int64_t)pattern_ms[k % (sizeof pattern_ms / sizeof pattern_ms[0])] *
                         (int64_t)SELFTEST_MS_NS;
        uint64_t now = SELFTEST_T0_NS + (uint64_t)k * period + (uint64_t)(jitter + 4 * (int64_t)SELFTEST_MS_NS);
        presented += (unsigned)present_slot_due_at(now);
    }
    assert(presented == 600u);

    /* The same with pseudo-random jitter of up to a fifth of a period either way. */
    selftest_reset_slots(NULL);
    uint64_t rng = 0x2c9277b5u;
    presented = 0;
    for (unsigned k = 0; k < 3600u; k++) {
        uint64_t span = (period * 2u) / 5u;
        uint64_t offset = (uint64_t)selftest_next_random(&rng) % (span + 1u);
        uint64_t now = SELFTEST_T0_NS + (uint64_t)k * period + offset;
        presented += (unsigned)present_slot_due_at(now);
    }
    assert(presented == 3600u);
}

/* Two flips in every scanout, a tenth and six tenths of the way through it: the cap still
 * presents no more than one per period over the run. */
static void selftest_two_flips_per_scanout_present_once_per_scanout(void) {
    const uint64_t period = PSP_SCANOUT_PERIOD_NS;
    selftest_reset_slots(NULL);
    unsigned presented = 0;
    const unsigned scanouts = 600u;
    for (unsigned k = 0; k < scanouts; k++) {
        uint64_t base = SELFTEST_T0_NS + (uint64_t)k * period;
        presented += (unsigned)present_slot_due_at(base + period / 10u);
        presented += (unsigned)present_slot_due_at(base + (period * 6u) / 10u);
    }
    assert(presented >= scanouts && presented <= scanouts + 1u);
}

/* Several submissions within one millisecond take one slot between them. */
static void selftest_burst_presents_once(void) {
    selftest_reset_slots(NULL);
    unsigned presented = 0;
    for (unsigned i = 0; i < 5u; i++)
        presented += (unsigned)present_slot_due_at(SELFTEST_T0_NS + (uint64_t)i * 200000ull);
    assert(presented == 1u);
}

/* An explicit 30 Hz cap over one jittered flip per scanout presents every other flip. */
static void selftest_explicit_cap_halves_scanout_flips(void) {
    static const int32_t pattern_ms[] = { 3, -2, 1, -3, 0, 2, -1, -3, 3, -2 };
    const uint64_t period = PSP_SCANOUT_PERIOD_NS;
    selftest_reset_slots("30");
    unsigned presented = 0;
    for (unsigned k = 0; k < 600u; k++) {
        int64_t jitter = (int64_t)pattern_ms[k % (sizeof pattern_ms / sizeof pattern_ms[0])] *
                         (int64_t)SELFTEST_MS_NS;
        uint64_t now = SELFTEST_T0_NS + (uint64_t)k * period + (uint64_t)(jitter + 4 * (int64_t)SELFTEST_MS_NS);
        presented += (unsigned)present_slot_due_at(now);
    }
    assert(presented >= 299u && presented <= 301u);
}

/* The unset SR_FPS_CAP default is the PSP scanout period, not a slower title-specific
 * rate: a guest may submit a framebuffer several times per scanout and every submission
 * past the next output slot is dropped rather than delaying the guest. */
int main(void) {
    s_present_cap = present_cap_from_value(NULL);
    assert(PSP_SCANOUT_PERIOD_NS == 16683333ull);
    assert(present_period_ns() == PSP_SCANOUT_PERIOD_NS);
    assert(present_period_ns() != 1000000000ull / 30u);
    assert(present_cap_from_value("native") == PRESENT_CAP_SCANOUT);
    assert(present_cap_from_value("-1") == PRESENT_CAP_SCANOUT);
    assert(present_cap_from_value("invalid") == PRESENT_CAP_SCANOUT);
    /* Through the slot logic: the first submission takes the slot, a second one in the
     * same scanout is dropped. */
    assert(present_slot_due());
    assert(!present_slot_due());

    s_present_cap = present_cap_from_value("0");
    assert(present_slot_due());

    s_present_cap = present_cap_from_value("30");
    assert(present_period_ns() == 1000000000ull / 30u);

    selftest_jittered_scanout_flips_are_all_presented();
    selftest_two_flips_per_scanout_present_once_per_scanout();
    selftest_burst_presents_once();
    selftest_explicit_cap_halves_scanout_flips();
    return 0;
}
#endif
