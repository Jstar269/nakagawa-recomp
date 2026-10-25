/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The in-game overlay's pages (#1159, C9c) on the SDL-free nk_ui core: page flow,
   focus, the actions each control returns, the layout at every supported window
   size, and draw-list goldens (fixtures/nk_ui/overlay.golden). Synthetic models
   only: no game, no SDL, no fonts. Run with --write-goldens after an intended
   change and say in the pull request why the golden moved. */

#include "nk_overlay.h"
#include "nk_ui_draw.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DUMP_CAP 1048576u
#define CASE_DUMP_CAP 131072u
#define GOLDEN_PATH "fixtures/nk_ui/overlay.golden"

static int failures;
static const uint8_t k_test_glyph_pixel = 0xFFu;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        ++failures; \
    } \
} while (0)

static bool test_raster(void *user, int font, int px, uint32_t glyph, NkUiGlyphBitmap *out) {
    (void)user;
    (void)font;
    (void)glyph;
    if (out == NULL || px <= 0) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = &k_test_glyph_pixel;
    out->width = 1;
    out->height = 1;
    out->pitch = 1;
    out->bearing_y = 1;
    out->advance_x = (float)px * 0.5f;
    return true;
}

static NkUiTextBackend g_text;

static NkUi *overlay_ui(int width, int height, float display_scale) {
    NkUiConfig config;
    NkUi *ui;
    NkUiViewport viewport = {width, height, display_scale, 0, 100};
    g_text = nk_ui_text_fixed();
    g_text.raster = test_raster;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &g_text;
    ui = nk_ui_create(&config);
    if (ui != NULL) nk_ui_set_viewport(ui, &viewport);
    return ui;
}

static const NkOverlayControlRow k_controls[] = {
    {"D-pad", "Arrow keys", "D-pad"},
    {"Analog stick", "", "Left stick"},
    {"Cross", "X", "A (south)"},
    {"Circle", "Z", "B (east)"},
    {"Square", "A", "X (west)"},
    {"Triangle", "S", "Y (north)"},
    {"L", "Q", "Left shoulder"},
    {"R", "W", "Right shoulder"},
    {"Start", "Enter", "Start"},
    {"Select", "Shift", "Back"}
};

static NkOverlayModel base_model(float width, float height) {
    NkOverlayModel model;
    memset(&model, 0, sizeof(model));
    model.logical_width = width;
    model.logical_height = height;
    model.title_name = "Synthetic Showcase Scene";
    model.disc_id = "TEST00007";
    model.scaling = NK_OVERLAY_SCALING_SHARP;
    model.pad_connected = true;
    model.controls = k_controls;
    model.control_count = sizeof(k_controls) / sizeof(k_controls[0]);
    return model;
}

static void push(NkUi *ui, NkUiEventType type, NkUiNavDir dir) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.dir = dir;
    event.down = true;
    nk_ui_push_event(ui, &event);
}

/* One frame, after `type` (when not -1) was queued. */
static NkOverlayAction frame_with(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model,
                                  int type, NkUiNavDir dir) {
    NkOverlayAction action;
    if (type >= 0) push(ui, (NkUiEventType)type, dir);
    nk_ui_begin(ui, 0.0);
    action = nk_overlay_frame(ui, state, model);
    CHECK(nk_ui_end(ui) != NULL, "an overlay frame completes");
    return action;
}

/* Focus moves use the rects laid out one frame earlier, so a page that just
   appeared needs two frames before directions find their targets (the host builds
   them back to back after a page change). */
static NkOverlayAction settle(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model) {
    (void)frame_with(ui, state, model, -1, NK_UI_DIR_NONE);
    return frame_with(ui, state, model, -1, NK_UI_DIR_NONE);
}

static bool list_has(const NkUiDrawList *list, NkUiId id) {
    for (uint32_t i = 0u; list != NULL && i < list->ncmd; ++i) {
        if (list->cmd[i].element_id == id) return true;
    }
    return false;
}

static bool frame_has(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model, NkUiId id) {
    const NkUiDrawList *list;
    nk_ui_begin(ui, 0.0);
    (void)nk_overlay_frame(ui, state, model);
    list = nk_ui_end(ui);
    return list_has(list, id);
}

static bool frame_has_text(NkUi *ui, NkOverlayState *state, const NkOverlayModel *model,
                           const char *needle) {
    static char dump[CASE_DUMP_CAP];
    nk_ui_begin(ui, 0.0);
    (void)nk_overlay_frame(ui, state, model);
    if (nk_ui_end(ui) == NULL) return false;
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    return strstr(dump, needle) != NULL;
}

static void test_menu(void) {
    NkUi *ui = overlay_ui(1280, 800, 1.0f);
    NkOverlayModel model = base_model(1280.0f, 800.0f);
    NkOverlayState state;
    NkOverlayAction action;
    CHECK(ui != NULL, "overlay context");
    if (ui == NULL) return;
    memset(&state, 0, sizeof(state));
    action = settle(ui, &state, &model);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE, "a closed overlay draws nothing and asks nothing");
    nk_overlay_open(&state, NK_OVERLAY_OPEN_USER);
    CHECK(state.open && state.screen == NK_OVERLAY_SCREEN_MENU, "Esc, F1 or Guide opens the menu");
    (void)settle(ui, &state, &model);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_RESUME, "RESUME is the menu's default focus");
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_RESUME, "Confirm on RESUME resumes");
    action = frame_with(ui, &state, &model, NK_UI_EV_BACK, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_RESUME, "Back on the menu resumes");
    action = frame_with(ui, &state, &model, NK_UI_EV_MENU, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_RESUME, "F1 or Start on the menu resumes");
    (void)frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_DOWN);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_SETTINGS, "down moves from RESUME to SETTINGS");
    (void)frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_DOWN);
    (void)frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_DOWN);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_QUIT, "QUIT TO LIBRARY is the last menu entry");
    model.title_name = NULL;
    CHECK(frame_has_text(ui, &state, &model, "\"Paused\""), "an unnamed game's menu says Paused");
    nk_overlay_close(&state);
    CHECK(!state.open, "closing clears the overlay");
    nk_ui_destroy(ui);
}

static void test_settings(void) {
    NkUi *ui = overlay_ui(1280, 800, 1.0f);
    NkOverlayModel model = base_model(1280.0f, 800.0f);
    NkOverlayState state;
    NkOverlayAction action;
    if (ui == NULL) return;
    nk_overlay_open(&state, NK_OVERLAY_OPEN_USER);
    (void)settle(ui, &state, &model);
    (void)frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_DOWN);
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE && state.screen == NK_OVERLAY_SCREEN_SETTINGS,
          "SETTINGS opens the settings page");
    (void)settle(ui, &state, &model);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_SCALING, "the scaling row is the default focus");

    /* Left and right belong to the segmented control: they change the value and
       focus stays on the row. */
    action = frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_RIGHT);
    CHECK(action.kind == NK_OVERLAY_ACT_SET_SCALING && action.value == NK_OVERLAY_SCALING_INTEGER,
          "right on Sharp selects Integer");
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_SCALING, "right keeps focus on the scaling row");
    action = frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_LEFT);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE, "left on the first option changes nothing");
    model.scaling = NK_OVERLAY_SCALING_SMOOTH;
    action = frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_RIGHT);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE, "right on the last option changes nothing");
    action = frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_LEFT);
    CHECK(action.kind == NK_OVERLAY_ACT_SET_SCALING && action.value == NK_OVERLAY_SCALING_INTEGER,
          "left on Smooth selects Integer");
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_SET_SCALING && action.value == NK_OVERLAY_SCALING_SHARP,
          "Confirm steps the scaling on, wrapping to Sharp");

    (void)frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_DOWN);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_HUD, "down moves to the HUD row");
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_SET_HUD && action.value == 1, "Confirm turns the HUD on");
    model.hud_on = true;
    action = frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_RIGHT);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE, "right on a HUD that is on changes nothing");
    action = frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_LEFT);
    CHECK(action.kind == NK_OVERLAY_ACT_SET_HUD && action.value == 0, "left turns the HUD off");
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_HUD, "left keeps focus on the HUD row");

    action = frame_with(ui, &state, &model, NK_UI_EV_BACK, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE && state.screen == NK_OVERLAY_SCREEN_MENU,
          "Back on settings returns to the menu, not the game");
    (void)settle(ui, &state, &model);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_SETTINGS, "the menu comes back focused on SETTINGS");
    nk_ui_destroy(ui);
}

static void test_controls(void) {
    NkUi *ui = overlay_ui(1280, 800, 1.0f);
    NkOverlayModel model = base_model(1280.0f, 800.0f);
    NkOverlayState state;
    NkOverlayAction action;
    if (ui == NULL) return;
    nk_overlay_open(&state, NK_OVERLAY_OPEN_USER);
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_CONTROLS, 0u);
    CHECK(frame_has_text(ui, &state, &model, "\"Right shoulder\""),
          "the controls page lists the bindings it is given");
    CHECK(frame_has_text(ui, &state, &model, "\"\xE2\x80\x94\""),
          "an unbound control shows a dash");
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_CONTROLS_BACK, "BACK is the controls page's focus");
    action = frame_with(ui, &state, &model, NK_UI_EV_BACK, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE && state.screen == NK_OVERLAY_SCREEN_MENU,
          "Back on controls returns to the menu");
    (void)settle(ui, &state, &model);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_CONTROLS, "the menu comes back focused on CONTROLS");
    model.control_count = 0u;
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_CONTROLS, 0u);
    CHECK(frame_has_text(ui, &state, &model, "No controls are mapped"),
          "an empty map says so instead of drawing an empty table");
    nk_ui_destroy(ui);
}

static void test_quit(void) {
    NkUi *ui = overlay_ui(1280, 800, 1.0f);
    NkOverlayModel model = base_model(1280.0f, 800.0f);
    NkOverlayState state;
    NkOverlayAction action;
    if (ui == NULL) return;
    nk_overlay_open(&state, NK_OVERLAY_OPEN_USER);
    nk_ui_focus(ui, NK_OVERLAY_ID_QUIT);
    (void)settle(ui, &state, &model);
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE && state.screen == NK_OVERLAY_SCREEN_QUIT,
          "QUIT TO LIBRARY asks first");
    (void)settle(ui, &state, &model);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_QUIT_STAY,
          "the quit dialog starts on KEEP PLAYING, never on the destructive action");
    CHECK(frame_has_text(ui, &state, &model, "Quit Synthetic Showcase Scene?"),
          "the quit dialog names the game");
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_RESUME, "KEEP PLAYING resumes");
    (void)frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_RIGHT);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_QUIT_CONFIRM, "right reaches QUIT TO LIBRARY");
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_QUIT, "confirming quits");
    action = frame_with(ui, &state, &model, NK_UI_EV_BACK, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE && state.screen == NK_OVERLAY_SCREEN_MENU,
          "Back on the quit dialog keeps the game paused, on the menu");
    (void)settle(ui, &state, &model);
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_QUIT, "the menu comes back focused on QUIT");

    /* While the game saves, the dialog offers no way to quit. */
    model.saving = true;
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_QUIT, 0u);
    CHECK(!frame_has(ui, &state, &model, NK_OVERLAY_ID_QUIT_CONFIRM),
          "no quit button while the game is saving");
    CHECK(frame_has_text(ui, &state, &model, "The game is saving"), "the saving dialog says why");
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_RESUME, "the saving dialog starts on RESUME");
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_RESUME, "RESUME lets the save finish");
    for (int i = 0; i < 4; ++i) {
        action = frame_with(ui, &state, &model, NK_UI_EV_NAV, NK_UI_DIR_RIGHT);
        CHECK(action.kind != NK_OVERLAY_ACT_QUIT, "nothing on the saving dialog quits");
        action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
        CHECK(action.kind != NK_OVERLAY_ACT_QUIT, "nothing on the saving dialog quits");
        if (state.screen != NK_OVERLAY_SCREEN_QUIT) break;
    }
    nk_ui_destroy(ui);
}

static void test_pad_lost(void) {
    NkUi *ui = overlay_ui(1280, 800, 1.0f);
    NkOverlayModel model = base_model(1280.0f, 800.0f);
    NkOverlayState state;
    NkOverlayAction action;
    if (ui == NULL) return;
    model.pad_connected = false;
    nk_overlay_open(&state, NK_OVERLAY_OPEN_PAD_LOST);
    CHECK(state.screen == NK_OVERLAY_SCREEN_PAD_LOST, "a lost controller opens its dialog");
    CHECK(frame_has_text(ui, &state, &model, "CONTROLLER DISCONNECTED"),
          "the dialog says the controller went away");
    CHECK(nk_ui_focused(ui) == NK_OVERLAY_ID_PAD_RESUME, "the dialog starts on RESUME");
    model.pad_connected = true;
    CHECK(frame_has_text(ui, &state, &model, "CONTROLLER CONNECTED"),
          "a reconnected controller changes the dialog");
    action = frame_with(ui, &state, &model, NK_UI_EV_BACK, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_NONE && state.screen == NK_OVERLAY_SCREEN_MENU,
          "Back keeps the game paused: it goes to the menu");
    nk_overlay_open(&state, NK_OVERLAY_OPEN_PAD_LOST);
    (void)settle(ui, &state, &model);
    action = frame_with(ui, &state, &model, NK_UI_EV_ACTIVATE, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_RESUME, "RESUME on the dialog resumes");
    action = frame_with(ui, &state, &model, NK_UI_EV_MENU, NK_UI_DIR_NONE);
    CHECK(action.kind == NK_OVERLAY_ACT_RESUME, "F1 or Start resumes from the dialog too");
    nk_ui_destroy(ui);
}

/* Every page at the design's three capture sizes: nothing drawn outside the window,
   nothing under the prompt bar but the bar, and the card clear of the edges. */
static void test_fits_every_window(void) {
    static const struct { int w; int h; float scale; } sizes[] = {
        {960, 600, 1.0f}, {1280, 800, 1.0f}, {1920, 1200, 1.5f}
    };
    const NkUiTheme *theme = nk_ui_theme_n1();
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        NkUi *ui = overlay_ui(sizes[s].w, sizes[s].h, sizes[s].scale);
        NkOverlayModel model = base_model((float)sizes[s].w / sizes[s].scale,
                                          (float)sizes[s].h / sizes[s].scale);
        int bar_top = sizes[s].h - (int)(NK_UI_PROMPT_BAR_HEIGHT * sizes[s].scale + 0.5f);
        int edge = (int)(NK_UI_SAFE_AREA_MIN * sizes[s].scale + 0.5f);
        if (ui == NULL) continue;
        for (int screen = 0; screen < NK_OVERLAY_SCREEN_COUNT; ++screen) {
            NkOverlayState state;
            const NkUiDrawList *list;
            char label[96];
            bool inside = true;
            bool clear = true;
            bool found_card = false;
            nk_overlay_open(&state, NK_OVERLAY_OPEN_USER);
            state.screen = (NkOverlayScreen)screen;
            model.saving = screen == NK_OVERLAY_SCREEN_QUIT && s == 0u;
            nk_ui_begin(ui, 0.0);
            (void)nk_overlay_frame(ui, &state, &model);
            list = nk_ui_end(ui);
            CHECK(list != NULL, "every page completes at every window size");
            for (uint32_t i = 0u; list != NULL && i < list->ncmd; ++i) {
                const NkUiDrawCmd *cmd = &list->cmd[i];
                if (cmd->kind == NK_UI_DRAW_SCISSOR_START || cmd->kind == NK_UI_DRAW_SCISSOR_END) {
                    continue;
                }
                if (cmd->bounds[0] < 0 || cmd->bounds[1] < 0 ||
                    cmd->bounds[2] > sizes[s].w || cmd->bounds[3] > sizes[s].h) {
                    inside = false;
                }
                /* The card: the one strong-edged border. */
                if (cmd->kind == NK_UI_DRAW_BORDER && cmd->rgba_pm == theme->line_strong &&
                    cmd->bounds[2] - cmd->bounds[0] > sizes[s].w / 4) {
                    found_card = true;
                    if (cmd->bounds[0] < edge || cmd->bounds[1] < edge ||
                        cmd->bounds[2] > sizes[s].w - edge || cmd->bounds[3] > bar_top) {
                        clear = false;
                    }
                }
            }
            (void)snprintf(label, sizeof(label), "page %d at %dx%d", screen, sizes[s].w,
                           sizes[s].h);
            if (!inside) fprintf(stderr, "  %s draws outside the window\n", label);
            if (!found_card) fprintf(stderr, "  %s has no card\n", label);
            if (!clear) fprintf(stderr, "  %s card reaches an edge or the prompt bar\n", label);
            CHECK(inside && found_card && clear, "every page fits every supported window");
        }
        nk_ui_destroy(ui);
    }
}

static void bind(NkInputProfile *profile, NkPspButton button, NkBindingSourceType type, int index,
                 NkBindingSourceType secondary_type, int secondary_index) {
    profile->psp_buttons[button].primary.type = type;
    profile->psp_buttons[button].primary.index = index;
    profile->psp_buttons[button].secondary.type = secondary_type;
    profile->psp_buttons[button].secondary.index = secondary_index;
}

static const NkOverlayControlRow *row_for(const NkOverlayControls *controls, const char *psp) {
    size_t i;
    for (i = 0; i < controls->count; ++i) {
        if (strcmp(controls->rows[i].psp, psp) == 0) return &controls->rows[i];
    }
    return NULL;
}

static bool row_is(const NkOverlayControls *controls, const char *psp, const char *keyboard,
                   const char *controller) {
    const NkOverlayControlRow *row = row_for(controls, psp);
    if (row == NULL) return false;
    if (strcmp(row->keyboard, keyboard) != 0 || strcmp(row->controller, controller) != 0) {
        fprintf(stderr, "  %s: keyboard \"%s\", controller \"%s\"\n", psp, row->keyboard,
                row->controller);
        return false;
    }
    return true;
}

/* The rows come from the mapping in force, and the controller's buttons are named
   for the pad's family. */
static void test_describe_controls(void) {
    static const NkOverlayKey keys[] = {
        {NK_PSP_BTN_START_BIT, "Enter"}, {NK_PSP_BTN_SELECT_BIT, "Shift"},
        {NK_PSP_BTN_CROSS_BIT, "X"}, {NK_PSP_BTN_CIRCLE_BIT, "Z"},
        {NK_PSP_BTN_SQUARE_BIT, "A"}, {NK_PSP_BTN_TRIANGLE_BIT, "S"},
        {NK_PSP_BTN_LTRIGGER_BIT, "Q"}, {NK_PSP_BTN_RTRIGGER_BIT, "W"},
        {NK_PSP_BTN_UP_BIT, "Arrow keys"}, {NK_PSP_BTN_DOWN_BIT, "Arrow keys"},
        {NK_PSP_BTN_LEFT_BIT, "Arrow keys"}, {NK_PSP_BTN_RIGHT_BIT, "Arrow keys"}
    };
    NkInputProfile profile;
    static NkOverlayControls controls;
    memset(&profile, 0, sizeof(profile));
    profile.axes[NK_PSP_AXIS_ANALOG_X].host_axis = NK_HOST_AXIS_LEFTX;
    profile.axes[NK_PSP_AXIS_ANALOG_Y].host_axis = NK_HOST_AXIS_LEFTY;
    bind(&profile, NK_PSP_BTN_CROSS, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_SOUTH, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_CIRCLE, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_EAST, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_SQUARE, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_WEST, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_TRIANGLE, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_NORTH, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_LTRIGGER, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_LEFT_SHOULDER,
         NK_BINDING_HOST_TRIGGER, NK_HOST_AXIS_LEFT_TRIGGER);
    bind(&profile, NK_PSP_BTN_START, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_START, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_SELECT, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_BACK, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_UP, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_DPAD_UP, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_DOWN, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_DPAD_DOWN, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_LEFT, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_DPAD_LEFT, NK_BINDING_NONE, 0);
    bind(&profile, NK_PSP_BTN_RIGHT, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_DPAD_RIGHT,
         NK_BINDING_HOST_AXIS_POS, NK_HOST_AXIS_LEFTX);

    nk_overlay_describe_controls(&controls, &profile, NK_UI_PAD_LETTERS_A_SOUTH, keys,
                                 sizeof(keys) / sizeof(keys[0]));
    CHECK(controls.count == 10u, "describe: one row per PSP control");
    CHECK(row_is(&controls, "D-pad", "Arrow keys", "D-pad / Left stick"),
          "describe: the four arrows are named once; a stick binding joins the D-pad");
    CHECK(row_is(&controls, "Analog stick", "", "Left stick"),
          "describe: the stick follows the profile's axes and has no key");
    CHECK(row_is(&controls, "Cross", "X", "A"), "describe: Xbox-style south is A");
    CHECK(row_is(&controls, "L", "Q", "LB / LT"), "describe: primary then secondary");
    CHECK(row_is(&controls, "R", "W", ""), "describe: an unbound control is empty");
    CHECK(row_is(&controls, "Select", "Shift", "View"), "describe: Xbox-style back is View");

    nk_overlay_describe_controls(&controls, &profile, NK_UI_PAD_SHAPES, keys,
                                 sizeof(keys) / sizeof(keys[0]));
    CHECK(row_is(&controls, "Cross", "X", "Cross"), "describe: a PlayStation-style pad says Cross");
    CHECK(row_is(&controls, "L", "Q", "L1 / L2"), "describe: PlayStation shoulder and trigger");
    nk_overlay_describe_controls(&controls, &profile, NK_UI_PAD_LETTERS_A_EAST, keys,
                                 sizeof(keys) / sizeof(keys[0]));
    CHECK(row_is(&controls, "Cross", "X", "B"), "describe: a Switch-style pad's south is B");
    nk_overlay_describe_controls(&controls, &profile, NK_UI_PAD_NEUTRAL, keys,
                                 sizeof(keys) / sizeof(keys[0]));
    CHECK(row_is(&controls, "Circle", "Z", "East button"), "describe: an unknown pad names the position");

    bind(&profile, NK_PSP_BTN_CROSS, NK_BINDING_HOST_BUTTON, NK_HOST_BUTTON_EAST, NK_BINDING_NONE, 0);
    nk_overlay_describe_controls(&controls, &profile, NK_UI_PAD_LETTERS_A_SOUTH, keys,
                                 sizeof(keys) / sizeof(keys[0]));
    CHECK(row_is(&controls, "Cross", "X", "B"), "describe: a remapped button shows the new binding");
    nk_overlay_describe_controls(&controls, NULL, NK_UI_PAD_LETTERS_A_SOUTH, NULL, 0u);
    CHECK(row_is(&controls, "Cross", "", ""), "describe: no mapping leaves the row unbound");
}

static bool append_text(char *out, size_t cap, size_t *used, const char *text, size_t length) {
    if (length > cap || *used > cap - length) return false;
    memcpy(out + *used, text, length);
    *used += length;
    return true;
}

static bool append_case(char *out, size_t cap, size_t *used, const char *name, NkUi *ui,
                        NkOverlayState *state, const NkOverlayModel *model) {
    static char dump[CASE_DUMP_CAP];
    size_t dumped;
    int written = snprintf(dump, sizeof(dump), "--- %s ---\n", name);
    if (written < 0 || (size_t)written >= sizeof(dump) ||
        !append_text(out, cap, used, dump, (size_t)written)) {
        return false;
    }
    /* Two frames: the first settles focus on the page, the second is dumped. */
    nk_ui_begin(ui, 0.0);
    (void)nk_overlay_frame(ui, state, model);
    if (nk_ui_end(ui) == NULL) return false;
    nk_ui_begin(ui, 0.0);
    (void)nk_overlay_frame(ui, state, model);
    if (nk_ui_end(ui) == NULL) return false;
    dumped = nk_ui_dump(ui, dump, sizeof(dump));
    while (dumped > 0u && (dump[dumped - 1u] == '\n' || dump[dumped - 1u] == '\r')) --dumped;
    return dumped < sizeof(dump) && append_text(out, cap, used, dump, dumped) &&
           append_text(out, cap, used, "\n", 1u);
}

static bool build_golden(char *out, size_t cap, size_t *used) {
    NkUi *ui = overlay_ui(1280, 800, 1.0f);
    NkUi *small = overlay_ui(960, 600, 1.0f);
    NkOverlayModel model = base_model(1280.0f, 800.0f);
    NkOverlayModel narrow = base_model(960.0f, 600.0f);
    NkOverlayState state;
    bool ok = ui != NULL && small != NULL;
    nk_overlay_open(&state, NK_OVERLAY_OPEN_USER);
    ok = ok && append_case(out, cap, used, "menu", ui, &state, &model);
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_SETTINGS, 0u);
    ok = ok && append_case(out, cap, used, "settings-scaling-focused", ui, &state, &model);
    model.scaling = NK_OVERLAY_SCALING_SMOOTH;
    model.hud_on = true;
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_SETTINGS, NK_OVERLAY_ID_HUD);
    ok = ok && append_case(out, cap, used, "settings-smooth-hud-on", ui, &state, &model);
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_CONTROLS, 0u);
    ok = ok && append_case(out, cap, used, "controls", ui, &state, &model);
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_CONTROLS, 0u);
    ok = ok && append_case(out, cap, used, "controls-960x600", small, &state, &narrow);
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_QUIT, 0u);
    ok = ok && append_case(out, cap, used, "quit", ui, &state, &model);
    model.saving = true;
    nk_overlay_go(&state, NK_OVERLAY_SCREEN_QUIT, 0u);
    ok = ok && append_case(out, cap, used, "quit-while-saving", ui, &state, &model);
    model.pad_connected = false;
    nk_overlay_open(&state, NK_OVERLAY_OPEN_PAD_LOST);
    ok = ok && append_case(out, cap, used, "pad-lost", ui, &state, &model);
    model.pad_connected = true;
    ok = ok && append_case(out, cap, used, "pad-reconnected", ui, &state, &model);
    nk_ui_destroy(ui);
    nk_ui_destroy(small);
    return ok;
}

static bool write_golden(const char *path, const char *contents, size_t size) {
    FILE *file = fopen(path, "wb");
    bool ok;
    if (file == NULL) return false;
    ok = fwrite(contents, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static void compare_golden(const char *path, const char *actual, size_t actual_size) {
    static char expected[DUMP_CAP];
    FILE *file = fopen(path, "rb");
    size_t expected_size;
    size_t mismatch = 0u;
    if (file == NULL) {
        fprintf(stderr, "FAIL: could not read overlay golden %s\n", path);
        ++failures;
        return;
    }
    expected_size = fread(expected, 1u, sizeof(expected), file);
    fclose(file);
    if (expected_size == actual_size && memcmp(expected, actual, actual_size) == 0) return;
    while (mismatch < expected_size && mismatch < actual_size &&
           expected[mismatch] == actual[mismatch]) {
        ++mismatch;
    }
    fprintf(stderr, "FAIL: %s dump changed at byte %zu (expected=%zu actual=%zu)\n",
            path, mismatch, expected_size, actual_size);
    ++failures;
}

int main(int argc, char **argv) {
    static char golden[DUMP_CAP];
    size_t golden_size = 0u;
    bool write = argc > 1 && strcmp(argv[1], "--write-goldens") == 0;
    test_menu();
    test_settings();
    test_controls();
    test_quit();
    test_pad_lost();
    test_fits_every_window();
    test_describe_controls();
    CHECK(build_golden(golden, sizeof(golden), &golden_size), "build the overlay golden");
    if (write) {
        CHECK(write_golden(GOLDEN_PATH, golden, golden_size), "write the overlay golden");
    } else {
        compare_golden(GOLDEN_PATH, golden, golden_size);
    }
    if (failures != 0) {
        fprintf(stderr, "overlay screens: %d failure(s)\n", failures);
        return 1;
    }
    printf("overlay screens: PASS\n");
    return 0;
}
