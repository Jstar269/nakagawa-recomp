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
    NK_OVERLAY_SCREEN_COUNT
} NkOverlayScreen;

typedef enum {
    NK_OVERLAY_ACT_NONE = 0,
    NK_OVERLAY_ACT_RESUME,       /* close the overlay; the game goes on */
    NK_OVERLAY_ACT_QUIT,         /* confirmed: end the run, back to the library */
    NK_OVERLAY_ACT_SET_SCALING,  /* value: the new game scaling (NK_OVERLAY_SCALING_*) */
    NK_OVERLAY_ACT_SET_HUD       /* value: 1 shows the performance HUD, 0 hides it */
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

/* One keyboard key the window maps to PSP buttons, as the window's key table has
   it (sdl3vk_keyboard_key). Keys that share a label (the four arrow keys) are
   named once on a row. */
typedef struct {
    uint32_t psp_bits;
    const char *label;
} NkOverlayKey;

/* The controls page's rows with the text they point into. */
typedef struct {
    NkOverlayControlRow rows[NK_OVERLAY_MAX_CONTROL_ROWS];
    size_t count;
    char text[1536];
    size_t used;
} NkOverlayControls;

/* Describes the PSP controls from the mapping in force: the window's key table
   for the keyboard column and the input profile for the controller column, with
   the controller's buttons named for `family` (an Xbox-style pad says A, a
   PlayStation-style one Cross, an unknown one South button). A NULL profile
   leaves the controller column unbound. */
void nk_overlay_describe_controls(NkOverlayControls *out, const NkInputProfile *profile,
                                  NkUiPadFamily family, const NkOverlayKey *keys,
                                  size_t key_count);

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
} NkOverlayModel;

/* Which page is up and where focus returns. Zero-initialised is closed. */
typedef struct {
    bool open;
    NkOverlayScreen screen;
    NkUiId restore_focus;         /* focused on the next frame, then cleared */
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
#define NK_OVERLAY_ID_SCALING 0x70000201u      /* + 1 + NK_OVERLAY_SCALING_* per option */
#define NK_OVERLAY_ID_HUD 0x70000210u
#define NK_OVERLAY_ID_SETTINGS_BACK 0x70000220u
#define NK_OVERLAY_ID_CONTROLS_BACK 0x70000301u
#define NK_OVERLAY_ID_QUIT_STAY 0x70000401u
#define NK_OVERLAY_ID_QUIT_CONFIRM 0x70000402u
#define NK_OVERLAY_ID_PAD_RESUME 0x70000501u
#define NK_OVERLAY_ID_PAD_MENU 0x70000502u

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

#ifdef __cplusplus
}
#endif

#endif /* NK_OVERLAY_H */
