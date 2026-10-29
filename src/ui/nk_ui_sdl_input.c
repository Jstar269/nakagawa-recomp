/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* SDL3 edge of the input model: SDL_Event in, NkUiEvent out.

   The core of nk_ui stays SDL-free, so SDL is only included here and only when the
   build defines NK_UI_WITH_SDL3. Every other build gets inert stubs, which keeps
   native-core-tests free of SDL on hosted Linux. The mapping itself lives in
   nk_ui_input.c; this file only reads SDL fields. */

#include "nk_ui_input.h"

#include <math.h>
#include <string.h>

#ifdef NK_UI_WITH_SDL3
#include <SDL3/SDL.h>
#endif

bool nk_ui_sdl_input_available(void) {
#ifdef NK_UI_WITH_SDL3
    return true;
#else
    return false;
#endif
}

void nk_ui_sdl_input_init(NkUiSdlInput *state) {
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    nk_ui_input_init(&state->input);
    state->event_scale = 1.0f;
}

void nk_ui_sdl_input_set_event_scale(NkUiSdlInput *state, float scale) {
    if (state != NULL) {
        state->event_scale = isfinite(scale) && scale > 0.0f ? scale : 1.0f;
    }
}

#ifndef NK_UI_WITH_SDL3

NkUiButtonLabel nk_ui_sdl_button_label(int sdl_gamepad_button_label) {
    (void)sdl_gamepad_button_label;
    return NK_UI_LABEL_UNKNOWN;
}

size_t nk_ui_sdl_input_translate(NkUiSdlInput *state, const union SDL_Event *event,
                                 double now_s, NkUiEvent *out, size_t cap) {
    (void)state;
    (void)event;
    (void)now_s;
    (void)out;
    (void)cap;
    return 0u;
}

#else

NkUiButtonLabel nk_ui_sdl_button_label(int label) {
    switch (label) {
    case SDL_GAMEPAD_BUTTON_LABEL_A: return NK_UI_LABEL_A;
    case SDL_GAMEPAD_BUTTON_LABEL_B: return NK_UI_LABEL_B;
    case SDL_GAMEPAD_BUTTON_LABEL_X: return NK_UI_LABEL_X;
    case SDL_GAMEPAD_BUTTON_LABEL_Y: return NK_UI_LABEL_Y;
    case SDL_GAMEPAD_BUTTON_LABEL_CROSS: return NK_UI_LABEL_CROSS;
    case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE: return NK_UI_LABEL_CIRCLE;
    case SDL_GAMEPAD_BUTTON_LABEL_SQUARE: return NK_UI_LABEL_SQUARE;
    case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return NK_UI_LABEL_TRIANGLE;
    default: return NK_UI_LABEL_UNKNOWN;
    }
}

/* The family comes from the label printed on the south button, never from the pad name. */
static NkUiPadFamily nk_ui_sdl_family(SDL_JoystickID id) {
    SDL_Gamepad *pad = SDL_GetGamepadFromID(id);
    if (pad == NULL) {
        return NK_UI_PAD_NEUTRAL;
    }
    return nk_ui_pad_family_for_south_label(
        nk_ui_sdl_button_label((int)SDL_GetGamepadButtonLabel(pad, SDL_GAMEPAD_BUTTON_SOUTH)));
}

static float nk_ui_sdl_axis(Sint16 value) {
    float v = (float)value / 32767.0f;
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);
}

/* A trigger is a button that engages past NK_UI_STICK_ENGAGE and lets go
   below NK_UI_STICK_RELEASE, so a resting or half-held trigger does not
   flicker between pages. */
static size_t nk_ui_sdl_trigger(NkUiSdlInput *state, const SDL_GamepadAxisEvent *axis,
                                double now_s, NkUiEvent *out, size_t cap) {
    int side = axis->axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0 : 1;
    float value = nk_ui_sdl_axis(axis->value);
    bool down = state->trigger_down[side] ? value >= NK_UI_STICK_RELEASE
                                          : value > NK_UI_STICK_ENGAGE;
    if (down == state->trigger_down[side]) {
        return 0u;
    }
    state->trigger_down[side] = down;
    return nk_ui_input_button(&state->input,
                              side == 0 ? NK_UI_BTN_PAGE_PREV : NK_UI_BTN_PAGE_NEXT, down,
                              (int)axis->which, nk_ui_sdl_family(axis->which), now_s,
                              out, cap);
}

static size_t nk_ui_sdl_key(NkUiSdlInput *state, const SDL_KeyboardEvent *key, double now_s,
                            NkUiEvent *out, size_t cap) {
    NkUiInput *in = &state->input;
    bool down = key->down;
    size_t n = 0u;
    NkUiInputButton button;
    if (key->repeat) {
        return 0u; /* repeat timing belongs to nk_ui_input_poll */
    }
    if ((key->mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0 && down) {
        return 0u; /* Alt+Enter and friends stay with the host */
    }
    switch (key->key) {
    case SDLK_UP: button = NK_UI_BTN_UP; break;
    case SDLK_DOWN: button = NK_UI_BTN_DOWN; break;
    case SDLK_LEFT: button = NK_UI_BTN_LEFT; break;
    case SDLK_RIGHT: button = NK_UI_BTN_RIGHT; break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: button = NK_UI_BTN_CONFIRM; break;
    case SDLK_ESCAPE: button = NK_UI_BTN_BACK; break;
    case SDLK_SPACE: button = NK_UI_BTN_OPTIONS; break;
    case SDLK_D: button = NK_UI_BTN_DETAILS; break;
    case SDLK_Q: button = NK_UI_BTN_TAB_PREV; break;
    case SDLK_E: button = NK_UI_BTN_TAB_NEXT; break;
    case SDLK_F1: button = NK_UI_BTN_MENU; break;
    case SDLK_PAGEUP: button = NK_UI_BTN_PAGE_PREV; break;
    case SDLK_PAGEDOWN: button = NK_UI_BTN_PAGE_NEXT; break;
    case SDLK_TAB:
        if (!down) {
            /* Release both so a Shift taken off before Tab cannot leave one stuck down. */
            n += nk_ui_input_button(in, NK_UI_BTN_CYCLE_NEXT, false,
                                    NK_UI_DEVICE_KEYBOARD_MOUSE, NK_UI_PAD_KEYBOARD, now_s,
                                    out + n, cap - n);
            n += nk_ui_input_button(in, NK_UI_BTN_CYCLE_PREV, false,
                                    NK_UI_DEVICE_KEYBOARD_MOUSE, NK_UI_PAD_KEYBOARD, now_s,
                                    out + n, cap - n);
            return n;
        }
        button = (key->mod & SDL_KMOD_SHIFT) != 0 ? NK_UI_BTN_CYCLE_PREV
                                                   : NK_UI_BTN_CYCLE_NEXT;
        break;
    default:
        return 0u;
    }
    return nk_ui_input_button(in, button, down, NK_UI_DEVICE_KEYBOARD_MOUSE,
                              NK_UI_PAD_KEYBOARD, now_s, out, cap);
}

static size_t nk_ui_sdl_pad_button(NkUiSdlInput *state, const SDL_GamepadButtonEvent *b,
                                   double now_s, NkUiEvent *out, size_t cap) {
    NkUiInputButton button;
    switch (b->button) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: button = NK_UI_BTN_UP; break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: button = NK_UI_BTN_DOWN; break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: button = NK_UI_BTN_LEFT; break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: button = NK_UI_BTN_RIGHT; break;
    case SDL_GAMEPAD_BUTTON_SOUTH: button = NK_UI_BTN_CONFIRM; break;
    case SDL_GAMEPAD_BUTTON_EAST: button = NK_UI_BTN_BACK; break;
    case SDL_GAMEPAD_BUTTON_WEST: button = NK_UI_BTN_OPTIONS; break;
    case SDL_GAMEPAD_BUTTON_NORTH: button = NK_UI_BTN_DETAILS; break;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: button = NK_UI_BTN_TAB_PREV; break;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: button = NK_UI_BTN_TAB_NEXT; break;
    case SDL_GAMEPAD_BUTTON_START: button = NK_UI_BTN_MENU; break;
    default: return 0u; /* Guide and Back stay out of the UI; the overlay claims them */
    }
    return nk_ui_input_button(&state->input, button, b->down, (int)b->which,
                              nk_ui_sdl_family(b->which), now_s, out, cap);
}

static size_t nk_ui_sdl_wheel(NkUiSdlInput *state, const SDL_MouseWheelEvent *w,
                              NkUiEvent *out, size_t cap) {
    float flip = w->direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
    float px = w->mouse_x * state->event_scale;
    float py = w->mouse_y * state->event_scale;
    size_t n = 0u;
    state->wheel_acc_x += w->x * flip;
    state->wheel_acc_y += w->y * flip;
    if (state->wheel_acc_x > 8.0f) state->wheel_acc_x = 8.0f;
    if (state->wheel_acc_x < -8.0f) state->wheel_acc_x = -8.0f;
    if (state->wheel_acc_y > 8.0f) state->wheel_acc_y = 8.0f;
    if (state->wheel_acc_y < -8.0f) state->wheel_acc_y = -8.0f;
    /* One event per whole notch: smooth wheels and trackpads accumulate fractions. */
    while (n < cap && state->wheel_acc_y >= 1.0f) {
        n += nk_ui_input_wheel(&state->input, px, py, NK_UI_DIR_UP, out + n, cap - n);
        state->wheel_acc_y -= 1.0f;
    }
    while (n < cap && state->wheel_acc_y <= -1.0f) {
        n += nk_ui_input_wheel(&state->input, px, py, NK_UI_DIR_DOWN, out + n, cap - n);
        state->wheel_acc_y += 1.0f;
    }
    while (n < cap && state->wheel_acc_x >= 1.0f) {
        n += nk_ui_input_wheel(&state->input, px, py, NK_UI_DIR_RIGHT, out + n, cap - n);
        state->wheel_acc_x -= 1.0f;
    }
    while (n < cap && state->wheel_acc_x <= -1.0f) {
        n += nk_ui_input_wheel(&state->input, px, py, NK_UI_DIR_LEFT, out + n, cap - n);
        state->wheel_acc_x += 1.0f;
    }
    return n;
}

size_t nk_ui_sdl_input_translate(NkUiSdlInput *state, const union SDL_Event *event,
                                 double now_s, NkUiEvent *out, size_t cap) {
    size_t n = 0u;
    if (state == NULL || event == NULL || out == NULL || cap == 0u) {
        return 0u;
    }
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return nk_ui_sdl_key(state, &event->key, now_s, out, cap);
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return nk_ui_sdl_pad_button(state, &event->gbutton, now_s, out, cap);
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            state->stick_x = nk_ui_sdl_axis(event->gaxis.value);
        } else if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            state->stick_y = nk_ui_sdl_axis(event->gaxis.value);
        } else if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                   event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
            return nk_ui_sdl_trigger(state, &event->gaxis, now_s, out, cap);
        } else {
            return 0u;
        }
        return nk_ui_input_stick(&state->input, state->stick_x, state->stick_y,
                                 (int)event->gaxis.which, nk_ui_sdl_family(event->gaxis.which),
                                 now_s, out, cap);
    case SDL_EVENT_GAMEPAD_ADDED:
        return nk_ui_input_device_change(&state->input, (int)event->gdevice.which,
                                         nk_ui_sdl_family(event->gdevice.which), true, out, cap);
    case SDL_EVENT_GAMEPAD_REMOVED: {
        int device = (int)event->gdevice.which;
        NkUiPadFamily family = nk_ui_input_last_device(&state->input) == device
                                   ? nk_ui_input_last_family(&state->input)
                                   : NK_UI_PAD_NEUTRAL;
        if (nk_ui_input_last_device(&state->input) == device) {
            nk_ui_input_reset(&state->input);
            state->stick_x = 0.0f;
            state->stick_y = 0.0f;
            state->trigger_down[0] = false;
            state->trigger_down[1] = false;
        }
        return nk_ui_input_device_change(&state->input, device, family, false, out, cap);
    }
    case SDL_EVENT_MOUSE_MOTION:
        return nk_ui_input_pointer(&state->input, event->motion.x * state->event_scale,
                                   event->motion.y * state->event_scale,
                                   state->input.pointer_down, out, cap);
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event->button.button != SDL_BUTTON_LEFT) {
            return 0u;
        }
        return nk_ui_input_pointer(&state->input, event->button.x * state->event_scale,
                                   event->button.y * state->event_scale,
                                   event->button.down, out, cap);
    case SDL_EVENT_MOUSE_WHEEL:
        return nk_ui_sdl_wheel(state, &event->wheel, out, cap);
    case SDL_EVENT_TEXT_INPUT: {
        const char *cursor = event->text.text;
        const char *end;
        uint32_t cp;
        if (cursor == NULL) {
            return 0u;
        }
        end = cursor + strlen(cursor);
        while (n < cap && (cp = nk_ui_utf8_next(&cursor, end)) != 0u) {
            n += nk_ui_input_text(&state->input, cp, out + n, cap - n);
        }
        return n;
    }
    default:
        return 0u;
    }
}

#endif /* NK_UI_WITH_SDL3 */
