/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_INPUT_H
#define NK_UI_INPUT_H

/* Unified input model, SDL translation entry points and prompt glyphs.

   This header holds three things that share the same logical-button vocabulary:
   1. the SDL-free input model (nk_ui_input.c): repeat timing, stick hysteresis,
      last-device tracking and NkUiEvent construction;
   2. the prompt glyphs (nk_ui_prompts.c): the label-to-glyph-set table and the
      geometry of the four neutral prompt styles, drawn with the draw list;
   3. the SDL translation (nk_ui_sdl_input.c), declared here with forward
      declarations only so this header includes no SDL header. Without
      NK_UI_WITH_SDL3 the translation compiles to inert stubs and
      nk_ui_sdl_input_available() returns false.

   Time is always injected as seconds (double) so tests are deterministic. */

#include "nk_ui.h"
#include "nk_ui_draw.h"

/* ------------------------------------------------------------------ */
/* Repeat timing                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t initial_delay_ms;
    uint32_t slow_ms;
    uint32_t normal_ms;
    uint32_t fast_ms;
} NkUiRepeatConfig;

/* 400 ms before the first repeat, then 110, 70 or 45 ms for Slow, Normal or Fast. */
NkUiRepeatConfig nk_ui_repeat_config_default(void);
uint32_t nk_ui_repeat_interval_ms(const NkUiRepeatConfig *config, NkUiRepeatRate rate);

/* ------------------------------------------------------------------ */
/* Input model                                                          */
/* ------------------------------------------------------------------ */

/* Logical buttons every device maps onto. Confirm is positional SOUTH, Back is EAST,
   Options is WEST and Details is NORTH, whatever the pad prints on them. */
typedef enum {
    NK_UI_BTN_UP = 0,
    NK_UI_BTN_DOWN,
    NK_UI_BTN_LEFT,
    NK_UI_BTN_RIGHT,
    NK_UI_BTN_CONFIRM,
    NK_UI_BTN_BACK,
    NK_UI_BTN_OPTIONS,
    NK_UI_BTN_DETAILS,
    NK_UI_BTN_TAB_PREV,
    NK_UI_BTN_TAB_NEXT,
    NK_UI_BTN_MENU,
    NK_UI_BTN_CYCLE_NEXT,
    NK_UI_BTN_CYCLE_PREV,
    NK_UI_BTN_COUNT
} NkUiInputButton;

/* The keyboard and mouse share this device id; pads use their own nonzero id. */
#define NK_UI_DEVICE_KEYBOARD_MOUSE 0

/* Large enough for one call: a device change plus the action, or a stick that
   releases one direction and presses another. */
#define NK_UI_INPUT_MAX_EVENTS 4

/* Stick thresholds: a direction engages at 0.5 and releases below 0.35. */
#define NK_UI_STICK_ENGAGE 0.5f
#define NK_UI_STICK_RELEASE 0.35f

typedef struct {
    NkUiRepeatConfig config;
    NkUiRepeatRate rate;
    bool down[NK_UI_BTN_COUNT];
    bool dir_held[2][4];       /* [0] buttons and keys, [1] stick; UP, DOWN, LEFT, RIGHT */
    uint32_t dir_seq[4];       /* press order of the held directions */
    uint32_t seq;
    NkUiNavDir stick_dir;
    bool repeating;
    NkUiNavDir repeat_dir;
    double next_repeat_s;
    int repeat_device;
    NkUiPadFamily repeat_family;
    bool have_device;
    int last_device;
    NkUiPadFamily last_family;
    bool have_pointer;
    float pointer_x;
    float pointer_y;
    bool pointer_down;
} NkUiInput;

void nk_ui_input_init(NkUiInput *input);
void nk_ui_input_set_repeat_rate(NkUiInput *input, NkUiRepeatRate rate);
void nk_ui_input_set_repeat_config(NkUiInput *input, const NkUiRepeatConfig *config);
/* Releases everything held (focus loss, pad removed): no stale repeat survives. */
void nk_ui_input_reset(NkUiInput *input);

/* Every function below writes at most `cap` events to `out` and returns the count.
   Presses carry down=true. Releases are reported only for NK_UI_EV_ACTIVATE (down=false),
   for pressed-state drawing; the other events are press-only. A press of a button that
   is already down (an OS key repeat) is ignored: repeats come from nk_ui_input_poll. */
size_t nk_ui_input_button(NkUiInput *input, NkUiInputButton button, bool down,
                          int device, NkUiPadFamily family, double now_s,
                          NkUiEvent *out, size_t cap);
/* Analog stick, x right and y down, each in [-1, 1]. Becomes up/down/left/right
   presses and releases with hysteresis. Drift below the engage threshold never
   counts as device activity. */
size_t nk_ui_input_stick(NkUiInput *input, float x, float y, int device,
                         NkUiPadFamily family, double now_s,
                         NkUiEvent *out, size_t cap);
/* Pointer position in LOGICAL UI pixels. A position change emits NK_UI_EV_POINTER; so
   does a button change. down is the current primary-button state, so a press is the
   event whose down is true while the previous event's was false. Hover never focuses. */
size_t nk_ui_input_pointer(NkUiInput *input, float x, float y, bool down,
                           NkUiEvent *out, size_t cap);
/* One wheel notch at the pointer: dir is the scroll direction (UP, DOWN, LEFT or RIGHT). */
size_t nk_ui_input_wheel(NkUiInput *input, float x, float y, NkUiNavDir dir,
                         NkUiEvent *out, size_t cap);
size_t nk_ui_input_text(NkUiInput *input, uint32_t codepoint, NkUiEvent *out, size_t cap);
/* A device appeared (down=true) or went away (down=false) without any input. */
size_t nk_ui_input_device_change(NkUiInput *input, int device, NkUiPadFamily family,
                                 bool connected, NkUiEvent *out, size_t cap);
/* Repeat events that are due at now_s (NK_UI_EV_NAV with repeat=true). Call once per
   frame; a long stall coalesces instead of flooding. */
size_t nk_ui_input_poll(NkUiInput *input, double now_s, NkUiEvent *out, size_t cap);
/* When the next repeat is due; false when nothing is held. */
bool nk_ui_input_next_wake(const NkUiInput *input, double *at_s);

NkUiPadFamily nk_ui_input_last_family(const NkUiInput *input);
int nk_ui_input_last_device(const NkUiInput *input);

/* Decodes one UTF-8 code point from *cursor (advancing it). Returns 0 at end of input;
   a malformed sequence yields U+FFFD and advances one byte. */
uint32_t nk_ui_utf8_next(const char **cursor, const char *end);

/* ------------------------------------------------------------------ */
/* Prompt glyphs                                                        */
/* ------------------------------------------------------------------ */

/* What a pad prints on its south button, in the project's own neutral vocabulary.
   The SDL layer converts SDL_GetGamepadButtonLabel(pad, SOUTH) into this. */
typedef enum {
    NK_UI_LABEL_UNKNOWN = 0,
    NK_UI_LABEL_A,
    NK_UI_LABEL_B,
    NK_UI_LABEL_X,
    NK_UI_LABEL_Y,
    NK_UI_LABEL_CROSS,
    NK_UI_LABEL_CIRCLE,
    NK_UI_LABEL_SQUARE,
    NK_UI_LABEL_TRIANGLE,
    NK_UI_LABEL_COUNT
} NkUiButtonLabel;

/* The label-to-glyph-set table: A on the south button means "letters, A south"; B on
   the south button means "letters, A east"; anything else, including unknown, gets the
   neutral position set. Pad names are never matched. */
NkUiPadFamily nk_ui_pad_family_for_south_label(NkUiButtonLabel label);
/* A family picks its default prompt style (the "Match input" behaviour). */
NkUiPromptStyle nk_ui_prompt_style_for_family(NkUiPadFamily family);

typedef enum {
    NK_UI_PROMPT_ACT_CONFIRM = 0,
    NK_UI_PROMPT_ACT_BACK,
    NK_UI_PROMPT_ACT_OPTIONS,
    NK_UI_PROMPT_ACT_DETAILS,
    NK_UI_PROMPT_ACT_TAB_PREV,
    NK_UI_PROMPT_ACT_TAB_NEXT,
    NK_UI_PROMPT_ACT_MENU,
    NK_UI_PROMPT_ACT_MOVE,   /* the D-pad cross, in every style */
    NK_UI_PROMPT_ACT_COUNT
} NkUiPromptAction;

/* The text drawn inside the glyph (a letter or a key name), or NULL when the glyph is
   pure geometry. The strings are static. */
const char *nk_ui_prompt_text(NkUiPromptStyle style, NkUiPromptAction action);
/* Width of the glyph for a given height. Keycaps grow with their label. */
float nk_ui_prompt_width(NkUiPromptStyle style, NkUiPromptAction action, float height);

typedef struct {
    NkUiPromptStyle style;
    NkUiPromptAction action;
    uint32_t fg_rgba;     /* active dot, label text, cross, menu bars */
    uint32_t dim_rgba;    /* inactive dots, keycap edge */
    uint32_t face_rgba;   /* letter circle, keycap face, pill */
    int font;             /* font index for the label text */
} NkUiPromptSpec;

/* Draws one glyph into the buffer, fitted into rect (logical UI pixels, whole-pixel
   rounded through density like every other primitive). Returns false only when the
   buffer could not grow. */
bool nk_ui_prompt_draw(NkUiDrawBuffer *buffer, const NkUiPromptSpec *spec,
                       NkUiRect rect, float density, uint32_t element_id);

/* ------------------------------------------------------------------ */
/* SDL translation                                                      */
/* ------------------------------------------------------------------ */

union SDL_Event;

typedef struct {
    NkUiInput input;
    float event_scale;        /* window-event units to logical UI pixels */
    float stick_x;
    float stick_y;
    float wheel_acc_x;
    float wheel_acc_y;
} NkUiSdlInput;

/* True when the library was built against SDL3 (NK_UI_WITH_SDL3). */
bool nk_ui_sdl_input_available(void);
void nk_ui_sdl_input_init(NkUiSdlInput *state);
/* Multiplier from SDL mouse/wheel coordinates to logical UI pixels: the window pixel
   density divided by NkUiScale.density. Values that are not finite and positive reset to 1. */
void nk_ui_sdl_input_set_event_scale(NkUiSdlInput *state, float scale);
/* Converts an SDL_GetGamepadButtonLabel value (an SDL_GamepadButtonLabel) to the
   project vocabulary. Unknown values give NK_UI_LABEL_UNKNOWN. */
NkUiButtonLabel nk_ui_sdl_button_label(int sdl_gamepad_button_label);
/* Translates one SDL_Event. Returns the number of events written to out (zero for an
   event this layer does not use, an OS key repeat, or the fullscreen keys F11 and
   Alt+Enter, which stay with the host). Without SDL3 it always returns zero. */
size_t nk_ui_sdl_input_translate(NkUiSdlInput *state, const union SDL_Event *event,
                                 double now_s, NkUiEvent *out, size_t cap);

#endif /* NK_UI_INPUT_H */
