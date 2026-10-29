/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_OVERLAY_H
#define NK_OVERLAY_H

/* The in-game overlay (#1159, lane C9c): the pause menu, its settings and controls
   pages, the quit confirmation and the controller-disconnected dialog, drawn with
   nk_ui over the frozen game frame while the scheduler holds the game paused
   (SrPauseHost, src/rt/sched.c). The screens are SDL-free: the runtime host (gui.c)
   fills an NkOverlayModel, feeds translated input to the NkUi, and carries out the
   action the frame returns. docs/PLAYER_UI_DESIGN.md "In-game overlay (C9c)" is the
   visual spec. */

#include "nk_input_profile.h"
#include "nk_ui.h"
#include "nk_ui_widgets.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NK_OVERLAY_OPEN_USER = 0,  /* Esc, F1 or Guide */
    NK_OVERLAY_OPEN_PAD_LOST   /* the controller in use went away */
} NkOverlayOpenReason;

typedef enum {
    NK_OVERLAY_SCREEN_MENU = 0,
    NK_OVERLAY_SCREEN_SETTINGS,
    NK_OVERLAY_SCREEN_CONTROLS,
    NK_OVERLAY_SCREEN_QUIT,
    NK_OVERLAY_SCREEN_PAD_LOST,
    NK_OVERLAY_SCREEN_VRAM,      /* developer mode only (#314) */
    NK_OVERLAY_SCREEN_PROFILE,   /* developer mode only (#314) */
    NK_OVERLAY_SCREEN_COUNT
} NkOverlayScreen;

typedef enum {
    NK_OVERLAY_ACT_NONE = 0,
    NK_OVERLAY_ACT_RESUME,       /* close the overlay; the game goes on */
    NK_OVERLAY_ACT_QUIT,         /* confirmed: end the run, back to the library */
    NK_OVERLAY_ACT_SET_SCALING,  /* value: the new game scaling (NK_OVERLAY_SCALING_*) */
    NK_OVERLAY_ACT_SET_HUD,      /* value: 1 shows the performance HUD, 0 hides it */
    NK_OVERLAY_ACT_VRAM_VIEW     /* the VRAM viewer's window moved: decode it again */
} NkOverlayActionKind;

typedef struct {
    NkOverlayActionKind kind;
    int value;
} NkOverlayAction;

/* Game scaling, in the order of the runtime's SDL3VK_SCALING_* values. */
enum {
    NK_OVERLAY_SCALING_SHARP = 0,
    NK_OVERLAY_SCALING_INTEGER = 1,
    NK_OVERLAY_SCALING_SMOOTH = 2,
    NK_OVERLAY_SCALING_COUNT
};

/* One row of the controls page: the PSP control and what drives it. NULL or ""
   shows as an em dash (not bound). */
typedef struct {
    const char *psp;
    const char *keyboard;
    const char *controller;
} NkOverlayControlRow;

#define NK_OVERLAY_MAX_CONTROL_ROWS 16u

/* The controls page's rows with the text they point into. */
typedef struct {
    NkOverlayControlRow rows[NK_OVERLAY_MAX_CONTROL_ROWS];
    size_t count;
    char text[1536];
    size_t used;
} NkOverlayControls;

/* Describes the PSP controls from the input profile the window maps through: its
   keyboard keys (psp_keys) for the keyboard column and its pad bindings for the
   controller column, with the controller's buttons named for `family` (an
   Xbox-style pad says A, a PlayStation-style one Cross, an unknown one South
   button). A NULL profile is the window's default keyboard and leaves the
   controller column unbound. */
void nk_overlay_describe_controls(NkOverlayControls *out, const NkInputProfile *profile,
                                  NkUiPadFamily family);

/* ---- Developer pages (#314) ----------------------------------------------------------
   Offered only when the model says developer mode is on (the launcher's setting, passed to
   the runtime as SR_DEVELOPER_MODE). Both are read-only views over a paused game: the VRAM
   viewer shows guest VRAM the host decoded, the Profile page the nakagawa-perf-v1 counters
   the host formatted (src/rt/perf.h). Neither page reads guest memory or counters itself. */

/* PSP VRAM: 2 MiB at 0x04000000. */
#define NK_OVERLAY_VRAM_BASE 0x04000000u
#define NK_OVERLAY_VRAM_SIZE 0x00200000u
/* The largest window the viewer decodes (GE_VRAM_VIEW_MAX_* in src/rt/ge_shared.h). */
#define NK_OVERLAY_VRAM_MAX_SIDE 512u
#define NK_OVERLAY_VRAM_MAX_STRIDE 1024u
#define NK_OVERLAY_VRAM_STRIDE_STEP 32u
/* Formats the window can be read as: the GE's colour PSMs and DEPTH16. */
#define NK_OVERLAY_VRAM_DEPTH16 11u

typedef enum {
    NK_OVERLAY_VRAM_DISPLAY = 0,  /* the framebuffer on screen */
    NK_OVERLAY_VRAM_DRAW,         /* the GE's render target */
    NK_OVERLAY_VRAM_DEPTH,        /* the GE's depth buffer */
    NK_OVERLAY_VRAM_TEXTURE,      /* level 0 of the texture the last draw bound */
    NK_OVERLAY_VRAM_SOURCE_COUNT,
    NK_OVERLAY_VRAM_CUSTOM = NK_OVERLAY_VRAM_SOURCE_COUNT  /* a window the person moved */
} NkOverlayVramSource;

/* A window of guest memory: address, row stride and size in pixels, and the format (a GE
   colour PSM 0-3, NK_OVERLAY_VRAM_DEPTH16, or for the texture source its own PSM 0-10). */
typedef struct {
    uint32_t addr;
    uint32_t stride;
    uint32_t width;
    uint32_t height;
    uint32_t format;
} NkOverlayVramSurface;

/* What the host knows of VRAM while the game is paused. */
typedef struct {
    NkOverlayVramSurface sources[NK_OVERLAY_VRAM_SOURCE_COUNT];
    bool present[NK_OVERLAY_VRAM_SOURCE_COUNT];  /* the GE state names that surface */
    bool texture_swizzled;
    const NkUiImage *image;   /* the window decoded; NULL when it could not be read */
    const char *note;         /* why there is no image, or a caveat; NULL for none */
} NkOverlayVram;

/* The viewer's window, kept in the overlay state while the game stays paused. */
typedef struct {
    int source;               /* NkOverlayVramSource */
    NkOverlayVramSurface view;
    bool chosen;              /* false: start from the display framebuffer */
} NkOverlayVramView;

/* Bytes per pixel of a window format (8888: 4, the rest 2). */
uint32_t nk_overlay_vram_bpp(uint32_t format);
/* Keeps a window inside the 2 MiB of VRAM: an address outside VRAM (other than its mirrors,
   which fold onto it) moves to its base, the address aligns to a pixel, an unknown format
   reads as 5650, stride, width and height stay within the viewer's limits (width at most the
   stride), and then the height, and if one row still does not fit the width, shrink until the
   last pixel is inside VRAM. Always leaves a window of at least one pixel. */
void nk_overlay_vram_clamp(NkOverlayVramSurface *view);
/* Moves the window one window-size (stride x height pixels) forward (dir > 0) or back
   (dir < 0). A step that would leave VRAM stops at its first or last whole window. */
void nk_overlay_vram_step(NkOverlayVramSurface *view, int dir);
/* "5650", "5551", "4444", "8888", "CLUT4" ... "DXT5", "DEPTH16"; "UNKNOWN" otherwise. */
const char *nk_overlay_vram_format_name(uint32_t format);

/* The Profile page: labelled rows and a bar per closed telemetry interval. */
#define NK_OVERLAY_PROFILE_ROWS 10u
#define NK_OVERLAY_PROFILE_BARS 60u
typedef enum {
    NK_OVERLAY_PROFILE_CPU = 0,   /* the PERF line's cpu_ms share of the interval */
    NK_OVERLAY_PROFILE_GE_WAIT,
    NK_OVERLAY_PROFILE_PRESENT,
    NK_OVERLAY_PROFILE_IDLE,
    NK_OVERLAY_PROFILE_PART_COUNT
} NkOverlayProfilePart;

typedef struct {
    const char *label;
    const char *value;
} NkOverlayProfileRow;

typedef struct {
    NkOverlayProfileRow rows[NK_OVERLAY_PROFILE_ROWS];
    size_t row_count;
    /* Oldest first; each part a share (0..1) of its interval's wall time. */
    float bars[NK_OVERLAY_PROFILE_BARS][NK_OVERLAY_PROFILE_PART_COUNT];
    size_t bar_count;
    const char *note;         /* a line under the rows; NULL for none */
} NkOverlayProfile;

/* What the screens show. The host fills it before every frame; the strings must
   outlive the frame. */
typedef struct {
    float logical_width;
    float logical_height;
    const char *title_name;      /* the game's name; NULL or "" shows no name */
    const char *disc_id;         /* NULL or "" shows none */
    int scaling;                 /* NK_OVERLAY_SCALING_* in use */
    bool hud_on;                 /* the performance HUD is shown in game */
    bool saving;                 /* a save is in progress: quitting is refused */
    bool pad_connected;          /* a controller is connected now */
    const NkOverlayControlRow *controls;
    size_t control_count;
    bool developer;              /* developer mode: the VRAM and Profile pages are offered */
    const NkOverlayVram *vram;   /* NULL: the VRAM page says there is nothing to show */
    const NkOverlayProfile *profile;
} NkOverlayModel;

/* Which page is up and where focus returns. Zero-initialised is closed. */
typedef struct {
    bool open;
    NkOverlayScreen screen;
    NkUiId restore_focus;         /* focused on the next frame, then cleared */
    NkOverlayVramView vram;       /* the VRAM viewer's window */
} NkOverlayState;

/* Opens on the menu, or on the controller dialog for NK_OVERLAY_OPEN_PAD_LOST. */
void nk_overlay_open(NkOverlayState *state, NkOverlayOpenReason reason);
void nk_overlay_close(NkOverlayState *state);

/* Builds one overlay frame between nk_ui_begin and nk_ui_end and returns the
   action the person chose this frame, if any. A page change is handled here and
   returns NK_OVERLAY_ACT_NONE. */
NkOverlayAction nk_overlay_frame(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model);

/* Labels shared by the pages and the tests. */
const char *nk_overlay_scaling_label(int scaling);

/* Element ids the host and tests address. */
#define NK_OVERLAY_ID_RESUME 0x70000101u
#define NK_OVERLAY_ID_SETTINGS 0x70000102u
#define NK_OVERLAY_ID_CONTROLS 0x70000103u
#define NK_OVERLAY_ID_QUIT 0x70000104u
#define NK_OVERLAY_ID_VRAM 0x70000105u          /* developer mode only */
#define NK_OVERLAY_ID_PROFILE 0x70000106u       /* developer mode only */
#define NK_OVERLAY_ID_SCALING 0x70000201u      /* + 1 + NK_OVERLAY_SCALING_* per option */
#define NK_OVERLAY_ID_HUD 0x70000210u
#define NK_OVERLAY_ID_SETTINGS_BACK 0x70000220u
#define NK_OVERLAY_ID_CONTROLS_BACK 0x70000301u
#define NK_OVERLAY_ID_QUIT_STAY 0x70000401u
#define NK_OVERLAY_ID_QUIT_CONFIRM 0x70000402u
#define NK_OVERLAY_ID_PAD_RESUME 0x70000501u
#define NK_OVERLAY_ID_PAD_MENU 0x70000502u
#define NK_OVERLAY_ID_VRAM_SOURCE 0x70000601u
#define NK_OVERLAY_ID_VRAM_ADDRESS 0x70000602u
#define NK_OVERLAY_ID_VRAM_STRIDE 0x70000603u
#define NK_OVERLAY_ID_VRAM_FORMAT 0x70000604u
#define NK_OVERLAY_ID_VRAM_BACK 0x70000610u
#define NK_OVERLAY_ID_PROFILE_BACK 0x70000701u

/* ---- Between the overlay's own files (nk_overlay.c and the overlay_*.c pages) ---- */

NkOverlayAction nk_overlay_action(NkOverlayActionKind kind, int value);
/* Shows `screen` and focuses `focus` (0: the page's default) on its first frame. */
void nk_overlay_go(NkOverlayState *state, NkOverlayScreen screen, NkUiId focus);
NkUiSizing nk_overlay_grow(void);
NkUiSizing nk_overlay_fit(void);
NkUiSizing nk_overlay_fixed(float value);
NkUiLayout nk_overlay_layout(NkUiLayoutDirection direction, NkUiSizing width,
                             NkUiSizing height, float gap);
/* Opens the page frame and its card at most card_width wide; returns the card's
   inner width, or 0 when nothing was opened (then nothing must be closed). */
float nk_overlay_page_begin(NkUi *ui, const NkOverlayModel *model, float card_width);
/* Closes the card and the page, then draws the prompt bar. */
void nk_overlay_page_end(NkUi *ui, const char *hint, const NkUiPromptItem *prompts,
                         size_t count);
/* The status line over a card's title: marker, a space, then the text, in caps. */
void nk_overlay_eyebrow(NkUi *ui, NkUiId id, const char *marker, const char *text,
                        uint32_t rgba);

NkOverlayAction nk_overlay_page_menu(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model);
NkOverlayAction nk_overlay_page_controls(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model);
NkOverlayAction nk_overlay_page_settings(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model);
NkOverlayAction nk_overlay_page_quit(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model);
NkOverlayAction nk_overlay_page_pad_lost(NkUi *ui, NkOverlayState *state,
                                         const NkOverlayModel *model);
NkOverlayAction nk_overlay_page_vram(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model);
NkOverlayAction nk_overlay_page_profile(NkUi *ui, NkOverlayState *state,
                                        const NkOverlayModel *model);

#ifdef __cplusplus
}
#endif

#endif /* NK_OVERLAY_H */
