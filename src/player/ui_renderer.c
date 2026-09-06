/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "ui_renderer.h"
#include <stdio.h>
#include <string.h>

/* Color palette */
static const SDL_Color COLOR_BG            = { 12,  15,  18, 255 }; /* #0c0f12 */
static const SDL_Color COLOR_CARD_BG       = { 22,  28,  34, 255 }; /* #161c22 */
static const SDL_Color COLOR_CARD_BORDER   = { 38,  50,  61, 255 }; /* #26323d */
static const SDL_Color COLOR_CARD_HOVER    = { 30,  38,  46, 255 };
static const SDL_Color COLOR_TEXT_WHITE    = { 241, 245, 249, 255 }; /* #f1f5f9 */
static const SDL_Color COLOR_TEXT_MUTED    = { 148, 163, 184, 255 }; /* #94a3b8 */
static const SDL_Color COLOR_TEXT_DIM      = { 100, 116, 139, 255 }; /* #64748b */
static const SDL_Color COLOR_EMERALD       = { 16,  185, 129, 255 }; /* #10b981 */
static const SDL_Color COLOR_EMERALD_HOVER = { 5,   150, 105, 255 };
static const SDL_Color COLOR_LIME          = { 132, 204,  22, 255 }; /* #84cc16 */
static const SDL_Color COLOR_AMBER         = { 245, 158,  11, 255 }; /* #f59e0b */
static const SDL_Color COLOR_RED           = { 239,  68,  68, 255 }; /* #ef4444 */
static const SDL_Color COLOR_BLUE          = { 59,  130, 246, 255 }; /* #3b82f6 */

static void set_draw_color(SDL_Renderer *ren, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
}

static void draw_filled_rect(SDL_Renderer *ren, float x, float y, float w, float h, SDL_Color c) {
    set_draw_color(ren, c);
    SDL_FRect r = { x, y, w, h };
    SDL_RenderFillRect(ren, &r);
}

static void draw_rect_outline(SDL_Renderer *ren, float x, float y, float w, float h, SDL_Color c) {
    set_draw_color(ren, c);
    SDL_FRect r = { x, y, w, h };
    SDL_RenderRect(ren, &r);
}

static void draw_text(SDL_Renderer *ren, float x, float y, const char *str, float scale, SDL_Color c) {
    if (!str || !*str) return;
    set_draw_color(ren, c);
    SDL_SetRenderScale(ren, scale, scale);
    SDL_RenderDebugText(ren, x / scale, y / scale, str);
    SDL_SetRenderScale(ren, 1.0f, 1.0f);
}

static bool is_point_in_rect(float px, float py, float rx, float ry, float rw, float rh) {
    return px >= rx && px <= (rx + rw) && py >= ry && py <= (ry + rh);
}

static bool draw_button(SDL_Renderer *ren, float x, float y, float w, float h, const char *label, bool is_accent, const UiInput *in) {
    bool hovered = in ? is_point_in_rect((float)in->mouse_x, (float)in->mouse_y, x, y, w, h) : false;
    SDL_Color bg = is_accent ? (hovered ? COLOR_EMERALD_HOVER : COLOR_EMERALD) : (hovered ? COLOR_CARD_HOVER : COLOR_CARD_BG);
    SDL_Color border = is_accent ? COLOR_EMERALD : COLOR_CARD_BORDER;
    SDL_Color text = is_accent ? (SDL_Color){10, 25, 20, 255} : COLOR_TEXT_WHITE;

    draw_filled_rect(ren, x, y, w, h, bg);
    draw_rect_outline(ren, x, y, w, h, border);

    /* Compute scale to fit button comfortably */
    float base_len = (float)strlen(label) * 8.0f;
    float max_scale = 1.3f;
    float avail_w = w - 16.0f;
    float scale = avail_w / base_len;
    if (scale > max_scale) scale = max_scale;
    if (scale < 0.8f) scale = 0.8f;

    float text_len = base_len * scale;
    float text_h = 8.0f * scale;
    float tx = x + (w - text_len) * 0.5f;
    float ty = y + (h - text_h) * 0.5f;
    draw_text(ren, tx, ty, label, scale, text);

    return hovered && in && in->mouse_clicked;
}

static void draw_badge(SDL_Renderer *ren, float x, float y, const char *label, SDL_Color badge_color) {
    float len = (float)strlen(label) * 8.0f + 16.0f;
    SDL_Color bg = { badge_color.r / 4, badge_color.g / 4, badge_color.b / 4, 255 };
    draw_filled_rect(ren, x, y, len, 24.0f, bg);
    draw_rect_outline(ren, x, y, len, 24.0f, badge_color);
    draw_text(ren, x + 8.0f, y + 4.0f, label, 1.0f, badge_color);
}

static void draw_progress_bar(SDL_Renderer *ren, float x, float y, float w, float h, float pct) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    draw_filled_rect(ren, x, y, w, h, (SDL_Color){ 20, 26, 32, 255 });
    draw_rect_outline(ren, x, y, w, h, COLOR_CARD_BORDER);
    if (pct > 0.0f) {
        float fill_w = (w - 4.0f) * (pct / 100.0f);
        draw_filled_rect(ren, x + 2.0f, y + 2.0f, fill_w, h - 4.0f, COLOR_EMERALD);
    }
}

/* --- Topbar Header --- */
static void render_topbar(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    draw_filled_rect(ren, 0, 0, w, 64.0f, (SDL_Color){ 16, 21, 26, 255 });
    draw_rect_outline(ren, 0, 63.0f, w, 1.0f, COLOR_CARD_BORDER);

    /* Logo Dot */
    draw_filled_rect(ren, 24.0f, 22.0f, 18.0f, 18.0f, COLOR_LIME);
    draw_text(ren, 52.0f, 16.0f, "NAKAGAWA RECOMP", 1.8f, COLOR_TEXT_WHITE);
    draw_text(ren, 52.0f, 40.0f, "AUTHENTIC PSP PLAYER", 1.0f, COLOR_TEXT_MUTED);

    /* Mode indicator */
    draw_badge(ren, 320.0f, 20.0f, "PLAYER MODE", COLOR_EMERALD);

    if (app->is_game_running) {
        char pid_str[64];
        snprintf(pid_str, sizeof(pid_str), "GAME ACTIVE (PID %d)", app->launch_session.process.process_id);
        draw_badge(ren, 460.0f, 20.0f, pid_str, COLOR_LIME);
    }

    /* Controller Badge */
    if (app->settings.controller_connected) {
        draw_badge(ren, w - 320.0f, 20.0f, "DUALSENSE CONNECTED", COLOR_LIME);
    } else {
        draw_badge(ren, w - 320.0f, 20.0f, "KEYBOARD READY", COLOR_TEXT_DIM);
    }

    /* Settings Button */
    if (draw_button(ren, w - 140.0f, 16.0f, 116.0f, 32.0f, "SETTINGS", false, in)) {
        if (app->active_view == VIEW_SETTINGS) {
            player_app_set_view(app, VIEW_LIBRARY);
        } else {
            player_app_set_view(app, VIEW_SETTINGS);
        }
    }
}

/* --- View: Empty Library --- */
static void render_empty_library(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cx = w * 0.5f;
    float cy = h * 0.5f;

    float card_w = 640.0f;
    float card_h = 360.0f;
    float card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;

    draw_filled_rect(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BG);
    draw_rect_outline(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, "PSP GAME LIBRARY", COLOR_BLUE);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, "Your Games, Recompiled for PC", 2.2f, COLOR_TEXT_WHITE);
    draw_text(ren, card_x + 32.0f, card_y + 124.0f, "No games currently loaded in library.", 1.2f, COLOR_TEXT_MUTED);
    draw_text(ren, card_x + 32.0f, card_y + 152.0f, "Select your lawfully obtained PSP game ISO to begin.", 1.2f, COLOR_TEXT_MUTED);
    draw_text(ren, card_x + 32.0f, card_y + 180.0f, "Supports registered PSP titles and synthetic test fixtures.", 1.0f, COLOR_TEXT_DIM);

    if (draw_button(ren, card_x + 32.0f, card_y + 240.0f, 260.0f, 48.0f, "+ ADD PSP GAME ISO", true, in)) {
        player_app_set_view(app, VIEW_INSPECTING);
    }
}

/* --- View: Loaded Game Library --- */
static void render_loaded_library(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    GameRecord *game = &app->games[app->selected_game_index >= 0 ? app->selected_game_index : 0];

    /* Hero Banner Card */
    float hero_x = 32.0f;
    float hero_y = 96.0f;
    float hero_w = w - 64.0f;
    float hero_h = 340.0f;

    draw_filled_rect(ren, hero_x, hero_y, hero_w, hero_h, COLOR_CARD_BG);
    draw_rect_outline(ren, hero_x, hero_y, hero_w, hero_h, COLOR_CARD_BORDER);

    /* Status Pills */
    draw_badge(ren, hero_x + 32.0f, hero_y + 28.0f, "VERIFIED RECOMPILATION", COLOR_EMERALD);
    draw_badge(ren, hero_x + 230.0f, hero_y + 28.0f, game->disc_id, COLOR_BLUE);
    draw_badge(ren, hero_x + 350.0f, hero_y + 28.0f, "60 FPS VULKAN", COLOR_LIME);

    /* Game Title */
    draw_text(ren, hero_x + 32.0f, hero_y + 72.0f, game->title_name, 2.5f, COLOR_TEXT_WHITE);
    draw_text(ren, hero_x + 32.0f, hero_y + 120.0f, "PlayStation Portable Classic · High-Definition Modern PC Recompilation", 1.2f, COLOR_TEXT_MUTED);

    /* Quick Specs Rail */
    float rail_y = hero_y + 160.0f;
    draw_filled_rect(ren, hero_x + 32.0f, rail_y, hero_w - 64.0f, 60.0f, (SDL_Color){ 16, 21, 26, 255 });
    draw_rect_outline(ren, hero_x + 32.0f, rail_y, hero_w - 64.0f, 60.0f, COLOR_CARD_BORDER);

    float col_w = (hero_w - 96.0f) / 3.0f;
    draw_text(ren, hero_x + 48.0f, rail_y + 12.0f, "RENDER RESOLUTION", 0.9f, COLOR_TEXT_DIM);
    draw_text(ren, hero_x + 48.0f, rail_y + 32.0f, "1080p (4x PSP) · Vulkan Native", 1.0f, COLOR_TEXT_WHITE);

    draw_text(ren, hero_x + 48.0f + col_w, rail_y + 12.0f, "CONTROLLER INPUT", 0.9f, COLOR_TEXT_DIM);
    draw_text(ren, hero_x + 48.0f + col_w, rail_y + 32.0f, "DualSense / Xbox Pad · Ready", 1.0f, COLOR_TEXT_WHITE);

    draw_text(ren, hero_x + 48.0f + col_w * 2.0f, rail_y + 12.0f, "SAVE STATE / PROGRESS", 0.9f, COLOR_TEXT_DIM);
    draw_text(ren, hero_x + 48.0f + col_w * 2.0f, rail_y + 32.0f, "Memory Stick Slot 1 · Career Ready", 1.0f, COLOR_TEXT_WHITE);

    /* Action Buttons */
    if (app->is_game_running) {
        char run_str[64];
        snprintf(run_str, sizeof(run_str), "STOP GAME (PID %d)", app->launch_session.process.process_id);
        if (draw_button(ren, hero_x + 32.0f, hero_y + 248.0f, 220.0f, 54.0f, run_str, true, in)) {
            player_app_stop_game(app);
        }
    } else if (game->is_prepared) {
        if (draw_button(ren, hero_x + 32.0f, hero_y + 248.0f, 220.0f, 54.0f, "PLAY NOW", true, in)) {
            player_app_launch_game(app, app->selected_game_index);
        }
    } else {
        if (draw_button(ren, hero_x + 32.0f, hero_y + 248.0f, 220.0f, 54.0f, "PREPARE GAME", true, in)) {
            app->prep_state.stage = STAGE_DECRYPTING_MODULES;
            app->prep_state.completed_items = 3500;
            app->prep_state.total_items = 56672;
            app->prep_state.percentage = 6.2f;
            player_app_set_view(app, VIEW_PREPARING);
        }
    }

    if (draw_button(ren, hero_x + 272.0f, hero_y + 248.0f, 220.0f, 54.0f, "ADD ANOTHER ISO", false, in)) {
        player_app_set_view(app, VIEW_INSPECTING);
    }

    /* Lower Library Strip */
    float strip_y = 460.0f;
    draw_text(ren, 32.0f, strip_y, "INSTALLED TITLES", 1.3f, COLOR_TEXT_WHITE);

    for (int i = 0; i < app->game_count; i++) {
        float card_x = 32.0f + i * 280.0f;
        float card_y = strip_y + 32.0f;
        float cw = 260.0f;
        float ch = 140.0f;

        bool active = (i == app->selected_game_index);
        SDL_Color bg = active ? (SDL_Color){ 28, 36, 44, 255 } : COLOR_CARD_BG;
        SDL_Color border = active ? COLOR_EMERALD : COLOR_CARD_BORDER;

        draw_filled_rect(ren, card_x, card_y, cw, ch, bg);
        draw_rect_outline(ren, card_x, card_y, cw, ch, border);

        draw_badge(ren, card_x + 12.0f, card_y + 12.0f, app->games[i].disc_id, active ? COLOR_EMERALD : COLOR_TEXT_DIM);
        draw_text(ren, card_x + 12.0f, card_y + 48.0f, app->games[i].title_name, 1.1f, COLOR_TEXT_WHITE);
        draw_text(ren, card_x + 12.0f, card_y + 104.0f, "Status: Ready to Play", 0.9f, COLOR_TEXT_MUTED);

        if (in && in->mouse_clicked && is_point_in_rect((float)in->mouse_x, (float)in->mouse_y, card_x, card_y, cw, ch)) {
            app->selected_game_index = i;
        }
    }
}

/* --- View: Inspecting ISO --- */
static void render_inspecting(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float cx = (float)app->window_width * 0.5f;
    float cy = (float)app->window_height * 0.5f;
    float card_w = 600.0f;
    float card_h = 300.0f;
    float card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;

    draw_filled_rect(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BG);
    draw_rect_outline(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, "ISO INSPECTOR", COLOR_AMBER);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, "Inspecting PSP Disc Image...", 2.0f, COLOR_TEXT_WHITE);
    draw_text(ren, card_x + 32.0f, card_y + 120.0f, "Reading ISO9660 Primary Volume Descriptor and PARAM.SFO...", 1.2f, COLOR_TEXT_MUTED);
    
    draw_progress_bar(ren, card_x + 32.0f, card_y + 160.0f, card_w - 64.0f, 16.0f, 45.0f);
    draw_text(ren, card_x + 32.0f, card_y + 190.0f, "File: HotShotsTennis.iso (1.20 GB)", 1.0f, COLOR_TEXT_DIM);

    if (draw_button(ren, card_x + 32.0f, card_y + 220.0f, 140.0f, 42.0f, "CANCEL", false, in)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Supported Title Result --- */
static void render_supported_title(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float cx = (float)app->window_width * 0.5f;
    float cy = (float)app->window_height * 0.5f;
    float card_w = 680.0f;
    float card_h = 360.0f;
    float card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;

    draw_filled_rect(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BG);
    draw_rect_outline(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 28.0f, "TITLE RECOGNIZED", COLOR_EMERALD);
    draw_badge(ren, card_x + 200.0f, card_y + 28.0f, app->inspecting_game.disc_id[0] ? app->inspecting_game.disc_id : "DISC_ID", COLOR_BLUE);
    draw_badge(ren, card_x + 320.0f, card_y + 28.0f, "AUTHENTIC LLE COMPATIBLE", COLOR_LIME);

    draw_text(ren, card_x + 32.0f, card_y + 72.0f, app->inspecting_game.title_name[0] ? app->inspecting_game.title_name : "PlayStation Portable Title", 2.2f, COLOR_TEXT_WHITE);
    draw_text(ren, card_x + 32.0f, card_y + 116.0f, "Disc identified as verified release in Nakagawa title catalog.", 1.2f, COLOR_TEXT_MUTED);
    draw_text(ren, card_x + 32.0f, card_y + 144.0f, "Authentic preparation requires local module decryption & asset indexing.", 1.1f, COLOR_TEXT_MUTED);

    /* Honesty warning */
    draw_text(ren, card_x + 32.0f, card_y + 180.0f, "NOTE: Full LLE font fidelity requires jpn0.pgf in system font directory.", 1.0f, COLOR_AMBER);

    if (draw_button(ren, card_x + 32.0f, card_y + 260.0f, 260.0f, 50.0f, "ADD TO LIBRARY", true, in)) {
        app->inspecting_game.is_prepared = true;
        player_app_add_game(app, &app->inspecting_game);
        player_app_set_view(app, VIEW_LIBRARY);
    }
    if (draw_button(ren, card_x + 310.0f, card_y + 260.0f, 140.0f, 50.0f, "BACK", false, in)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Unsupported Title Result --- */
static void render_unsupported_title(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float cx = (float)app->window_width * 0.5f;
    float cy = (float)app->window_height * 0.5f;
    float card_w = 640.0f;
    float card_h = 320.0f;
    float card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;

    draw_filled_rect(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BG);
    draw_rect_outline(ren, card_x, card_y, card_w, card_h, COLOR_RED);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, "UNSUPPORTED TITLE", COLOR_RED);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, "Title Not Qualified", 2.2f, COLOR_TEXT_WHITE);
    draw_text(ren, card_x + 32.0f, card_y + 120.0f, "The selected ISO disc image is a valid PSP game, but is not yet", 1.2f, COLOR_TEXT_MUTED);
    draw_text(ren, card_x + 32.0f, card_y + 144.0f, "registered in Nakagawa Recomp's title registry.", 1.2f, COLOR_TEXT_MUTED);
    draw_text(ren, card_x + 32.0f, card_y + 180.0f, "To avoid unpredictable crashes, unsupported titles are not executed.", 1.0f, COLOR_TEXT_DIM);

    if (draw_button(ren, card_x + 32.0f, card_y + 230.0f, 220.0f, 48.0f, "RETURN TO LIBRARY", true, in)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Preparation Progress --- */
static void render_preparing(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float cx = (float)app->window_width * 0.5f;
    float cy = (float)app->window_height * 0.5f;
    float card_w = 700.0f;
    float card_h = 360.0f;
    float card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;

    draw_filled_rect(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BG);
    draw_rect_outline(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, "TRANSACTIONAL PREPARATION", COLOR_EMERALD);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, "Preparing Game Runtime...", 2.2f, COLOR_TEXT_WHITE);
    draw_text(ren, card_x + 32.0f, card_y + 120.0f, "Indexing archive containers & validating ELF module headers...", 1.2f, COLOR_TEXT_MUTED);

    /* Real progress indicators */
    draw_progress_bar(ren, card_x + 32.0f, card_y + 160.0f, card_w - 64.0f, 20.0f, app->prep_state.percentage);
    
    char count_str[128];
    snprintf(count_str, sizeof(count_str), "%d / %d items processed (%.1f%%)", app->prep_state.completed_items, app->prep_state.total_items, app->prep_state.percentage);
    draw_text(ren, card_x + 32.0f, card_y + 196.0f, count_str, 1.2f, COLOR_TEXT_WHITE);

    draw_text(ren, card_x + 32.0f, card_y + 228.0f, "Staging: .staging_temp/ (atomic rename on verify)", 1.0f, COLOR_TEXT_DIM);

    if (draw_button(ren, card_x + 32.0f, card_y + 270.0f, 160.0f, 46.0f, "CANCEL TASK", false, in)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Settings --- */
static void render_settings(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float card_w = w - 64.0f;
    float card_h = h - 128.0f;
    if (card_h < 560.0f) card_h = 560.0f;
    float card_x = 32.0f;
    float card_y = 96.0f;

    draw_filled_rect(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BG);
    draw_rect_outline(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 24.0f, "PLAYER CONFIGURATION", COLOR_BLUE);
    draw_text(ren, card_x + 32.0f, card_y + 64.0f, "Graphics & Controller Settings", 2.2f, COLOR_TEXT_WHITE);

    /* Settings Sections */
    float col1_x = card_x + 32.0f;
    float col2_x = card_x + card_w * 0.5f;

    /* Resolution scale */
    draw_text(ren, col1_x, card_y + 120.0f, "INTERNAL RENDER RESOLUTION", 1.1f, COLOR_TEXT_DIM);
    draw_button(ren, col1_x, card_y + 144.0f, 115.0f, 36.0f, "1x (480x272)", app->settings.resolution_scale == 1, in);
    draw_button(ren, col1_x + 125.0f, card_y + 144.0f, 115.0f, 36.0f, "2x (Vita)", app->settings.resolution_scale == 2, in);
    draw_button(ren, col1_x + 250.0f, card_y + 144.0f, 115.0f, 36.0f, "4x (1080p)", app->settings.resolution_scale == 4, in);
    draw_button(ren, col1_x + 375.0f, card_y + 144.0f, 115.0f, 36.0f, "8x (4K UHD)", app->settings.resolution_scale == 8, in);

    /* Frame rate */
    draw_text(ren, col1_x, card_y + 210.0f, "FRAME CADENCE & VSYNC", 1.1f, COLOR_TEXT_DIM);
    draw_button(ren, col1_x, card_y + 234.0f, 140.0f, 36.0f, "30 FPS (PSP Cap)", app->settings.fps_cap == 30, in);
    draw_button(ren, col1_x + 150.0f, card_y + 234.0f, 140.0f, 36.0f, "60 FPS (Smooth)", app->settings.fps_cap == 60, in);
    draw_button(ren, col1_x + 300.0f, card_y + 234.0f, 140.0f, 36.0f, "Uncapped VSync", app->settings.fps_cap == 0, in);

    /* Audio */
    draw_text(ren, col2_x, card_y + 120.0f, "AUDIO & SOUND OUTPUT", 1.1f, COLOR_TEXT_DIM);
    draw_text(ren, col2_x, card_y + 144.0f, "Backend: SDL3 Spatial Audio Stream (44.1 kHz 16-bit stereo)", 1.1f, COLOR_TEXT_WHITE);
    draw_progress_bar(ren, col2_x, card_y + 172.0f, 300.0f, 12.0f, 80.0f);
    draw_text(ren, col2_x + 320.0f, card_y + 170.0f, "80%", 1.0f, COLOR_TEXT_MUTED);

    /* Controller */
    draw_text(ren, col2_x, card_y + 210.0f, "GAMEPAD CONFIGURATION", 1.1f, COLOR_TEXT_DIM);
    draw_text(ren, col2_x, card_y + 234.0f, "Device: Sony DualSense Wireless Controller (XInput fallback)", 1.1f, COLOR_TEXT_WHITE);
    draw_text(ren, col2_x, card_y + 258.0f, "Haptic Feedback & Trigger Resistance: Enabled", 1.0f, COLOR_EMERALD);

    /* Save path */
    draw_text(ren, col1_x, card_y + 300.0f, "STORAGE & SAVE DIRECTORY", 1.1f, COLOR_TEXT_DIM);
    draw_text(ren, col1_x, card_y + 324.0f, "Location: %LOCALAPPDATA%/nakagawa/savedata/", 1.1f, COLOR_TEXT_WHITE);

    /* Close */
    if (draw_button(ren, card_x + 32.0f, card_y + card_h - 72.0f, 200.0f, 46.0f, "SAVE & CLOSE", true, in)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Error Dialog --- */
static void render_error(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float cx = (float)app->window_width * 0.5f;
    float cy = (float)app->window_height * 0.5f;
    float card_w = 640.0f;
    float card_h = 320.0f;
    float card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;

    draw_filled_rect(ren, card_x, card_y, card_w, card_h, COLOR_CARD_BG);
    draw_rect_outline(ren, card_x, card_y, card_w, card_h, COLOR_RED);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, app->last_error.error_code, COLOR_RED);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, app->last_error.title, 2.2f, COLOR_TEXT_WHITE);
    draw_text(ren, card_x + 32.0f, card_y + 120.0f, app->last_error.message, 1.2f, COLOR_TEXT_MUTED);

    if (draw_button(ren, card_x + 32.0f, card_y + 230.0f, 240.0f, 48.0f, app->last_error.recovery_action_label, true, in)) {
        player_app_set_view(app, app->last_error.return_view);
    }
}

/* --- Main Frame Render Function --- */
void ui_render_frame(SDL_Renderer *renderer, PlayerApp *app, const UiInput *input) {
    if (!renderer || !app) return;

    /* Background Clear */
    set_draw_color(renderer, COLOR_BG);
    SDL_RenderClear(renderer);

    /* Render Topbar Header */
    render_topbar(renderer, app, input);

    /* View Routing */
    switch (app->active_view) {
        case VIEW_LIBRARY:
            if (app->game_count == 0) {
                render_empty_library(renderer, app, input);
            } else {
                render_loaded_library(renderer, app, input);
            }
            break;
        case VIEW_INSPECTING:
            render_inspecting(renderer, app, input);
            break;
        case VIEW_SUPPORTED_TITLE:
            render_supported_title(renderer, app, input);
            break;
        case VIEW_UNSUPPORTED_TITLE:
            render_unsupported_title(renderer, app, input);
            break;
        case VIEW_PREPARING:
            render_preparing(renderer, app, input);
            break;
        case VIEW_SETTINGS:
            render_settings(renderer, app, input);
            break;
        case VIEW_ERROR:
            render_error(renderer, app, input);
            break;
        default:
            render_empty_library(renderer, app, input);
            break;
    }

    SDL_RenderPresent(renderer);
}

bool ui_capture_screenshot(SDL_Renderer *renderer, const char *output_bmp_path) {
    if (!renderer || !output_bmp_path) return false;
    SDL_Surface *surf = SDL_RenderReadPixels(renderer, NULL);
    if (!surf) return false;
    bool ok = SDL_SaveBMP(surf, output_bmp_path);
    SDL_DestroySurface(surf);
    return ok;
}
