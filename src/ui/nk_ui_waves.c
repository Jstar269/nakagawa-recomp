/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui_waves.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static const uint32_t g_base_colors[NK_UI_WAVES_RIBBON_COUNT] = {
    0x5E9CC2FFu, 0x6E9600FFu, 0xD7F24AFFu, 0xE3F77AFFu
};

static const uint32_t g_month_tints[NK_UI_WAVES_TINT_COUNT] = {
    0x60A9A7FFu, 0x4FB093FFu, 0x54B67EFFu, 0x4AB558FFu,
    0x7BD85AFFu, 0xACE75FFFu, 0xD7F24AFFu, 0xD0CB39FFu,
    0x899B31FFu, 0x598137FFu, 0x548357FFu, 0x628D79FFu
};

static const double g_ribbon_base[NK_UI_WAVES_RIBBON_COUNT] = {
    0.64, 0.73, 0.80, 0.70
};
static const double g_ribbon_width[NK_UI_WAVES_RIBBON_COUNT] = {
    0.17, 0.12, 0.085, 0.055
};
static const double g_ribbon_depth[NK_UI_WAVES_RIBBON_COUNT] = {
    0.30, 0.55, 0.80, 1.00
};
static const double g_ribbon_seed[NK_UI_WAVES_RIBBON_COUNT] = {
    0.6, 2.1, 4.0, 5.3
};
static const double g_wave_amplitude[3] = {0.055, 0.030, 0.014};
static const double g_wave_frequency[3] = {0.32, 0.71, 1.37};
static const double g_wave_speed[3] = {0.090, -0.140, 0.230};
static const double g_wave_seed_factor[3] = {1.0, 1.7, 2.9};
static const double g_tau = 6.28318530717958647692;
static const uint32_t g_surface_rgba = 0x0B0F14FFu;

static uint32_t nk_ui_mix_half(uint32_t a, uint32_t b) {
    uint32_t r = (((a >> 24) & 0xFFu) + ((b >> 24) & 0xFFu) + 1u) / 2u;
    uint32_t g = (((a >> 16) & 0xFFu) + ((b >> 16) & 0xFFu) + 1u) / 2u;
    uint32_t blue = (((a >> 8) & 0xFFu) + ((b >> 8) & 0xFFu) + 1u) / 2u;
    return (r << 24) | (g << 16) | (blue << 8) | 0xFFu;
}

static void nk_ui_color_float(uint32_t rgba, float out[4]) {
    out[0] = (float)((rgba >> 24) & 0xFFu) / 255.0f;
    out[1] = (float)((rgba >> 16) & 0xFFu) / 255.0f;
    out[2] = (float)((rgba >> 8) & 0xFFu) / 255.0f;
    out[3] = (float)(rgba & 0xFFu) / 255.0f;
}

static float nk_ui_srgb_to_linear(float channel) {
    if (channel <= 0.04045f) {
        return channel / 12.92f;
    }
    return powf((channel + 0.055f) / 1.055f, 2.4f);
}

static float nk_ui_luminance(float r, float g, float b) {
    return 0.2126f * nk_ui_srgb_to_linear(r) +
           0.7152f * nk_ui_srgb_to_linear(g) +
           0.0722f * nk_ui_srgb_to_linear(b);
}

static float nk_ui_composite_luminance(uint32_t surface, uint32_t color, float alpha) {
    float s[4];
    float c[4];
    nk_ui_color_float(surface, s);
    nk_ui_color_float(color, c);
    return nk_ui_luminance(s[0] * (1.0f - alpha) + c[0] * alpha,
                           s[1] * (1.0f - alpha) + c[1] * alpha,
                           s[2] * (1.0f - alpha) + c[2] * alpha);
}

bool nk_ui_waves_palette(unsigned tint_index, NkUiWavePalette *out) {
    unsigned i;
    uint32_t tint;
    if (out == NULL || tint_index > NK_UI_WAVES_TINT_COUNT) {
        return false;
    }
    tint = tint_index == 0 ? 0u : g_month_tints[tint_index - 1u];
    for (i = 0; i < NK_UI_WAVES_RIBBON_COUNT; ++i) {
        out->rgba[i] = tint_index == 0 ? g_base_colors[i] :
                       nk_ui_mix_half(g_base_colors[i], tint);
    }
    out->cap = nk_ui_waves_compute_cap(out, g_surface_rgba);
    return true;
}

float nk_ui_waves_compute_cap(const NkUiWavePalette *palette, uint32_t surface_rgba) {
    float low = 0.0f;
    float high = NK_UI_WAVES_ALPHA_CAP_MAX;
    unsigned iteration;
    unsigned i;
    if (palette == NULL) {
        return 0.0f;
    }
    for (iteration = 0; iteration < 28u; ++iteration) {
        float middle = (low + high) * 0.5f;
        bool within_limit = true;
        for (i = 0; i < NK_UI_WAVES_RIBBON_COUNT; ++i) {
            if (nk_ui_composite_luminance(surface_rgba, palette->rgba[i], middle) >
                NK_UI_WAVES_LUMINANCE_MAX) {
                within_limit = false;
                break;
            }
        }
        if (within_limit) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}

bool nk_ui_waves_contrast_ok(const NkUiWavePalette *palette, uint32_t surface_rgba,
                             float luminance_limit) {
    float cap;
    unsigned i;
    if (palette == NULL || !isfinite(luminance_limit) || luminance_limit <= 0.0f) {
        return false;
    }
    cap = nk_ui_waves_compute_cap(palette, surface_rgba);
    if (cap < 0.0f || cap > NK_UI_WAVES_ALPHA_CAP_MAX + 1e-6f) {
        return false;
    }
    for (i = 0; i < NK_UI_WAVES_RIBBON_COUNT; ++i) {
        if (nk_ui_composite_luminance(surface_rgba, palette->rgba[i], cap) > luminance_limit) {
            return false;
        }
    }
    return true;
}

static double nk_ui_wrap_phase(double phase) {
    phase = fmod(phase, g_tau);
    if (phase < 0.0) {
        phase += g_tau;
    }
    return phase;
}

void nk_ui_waves_command(double now_s, float width_px, float height_px,
                         const NkUiWavePalette *palette, NkUiWavesCommand *out) {
    unsigned i;
    unsigned j;
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!isfinite(now_s)) {
        now_s = 37.0;
    }
    out->width_px = width_px > 0.0f ? width_px : 0.0f;
    out->height_px = height_px > 0.0f ? height_px : 0.0f;
    if (palette == NULL) {
        return;
    }
    out->cap = palette->cap;
    for (i = 0; i < NK_UI_WAVES_RIBBON_COUNT; ++i) {
        for (j = 0; j < 3u; ++j) {
            out->phase[i][j] = nk_ui_wrap_phase(
                g_wave_speed[j] * g_ribbon_depth[i] * now_s +
                g_wave_seed_factor[j] * g_ribbon_seed[i]);
        }
        out->width_phase[i] = nk_ui_wrap_phase(
            0.05 * g_ribbon_depth[i] * now_s + 1.3 * g_ribbon_seed[i]);
        nk_ui_color_float(palette->rgba[i], out->color_rgba[i]);
    }
}

static float nk_ui_smoothstep(float value) {
    float t = value;
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    return t * t * (3.0f - 2.0f * t);
}

static uint8_t nk_ui_to_byte(float value) {
    float scaled = value * 255.0f;
    if (scaled < 0.0f) {
        scaled = 0.0f;
    } else if (scaled > 255.0f) {
        scaled = 255.0f;
    }
    return (uint8_t)floorf(scaled + 0.5f);
}

bool nk_ui_waves_render_rgba(const NkUiWavePalette *palette, double now_s,
                             uint32_t width, uint32_t height, uint8_t *pixels,
                             size_t stride) {
    NkUiWavesCommand command;
    if (palette == NULL) {
        return false;
    }
    nk_ui_waves_command(now_s, (float)width, (float)height, palette, &command);
    return nk_ui_waves_render_command(&command, width, height, pixels, stride);
}

/* Shapes are measured in target heights, so a smaller target draws the same
   picture at lower resolution; the backend scales it up with a linear filter. */
bool nk_ui_waves_render_command(const NkUiWavesCommand *source, uint32_t width,
                                uint32_t height, uint8_t *pixels, size_t stride) {
    NkUiWavesCommand command;
    float colors[NK_UI_WAVES_RIBBON_COUNT][4];
    uint32_t x;
    uint32_t y;
    if (source == NULL || pixels == NULL || width == 0u || height == 0u ||
#if SIZE_MAX < UINT64_MAX
        (size_t)width > SIZE_MAX / 4u ||
#endif
        stride < (size_t)width * 4u ||
        (size_t)height > SIZE_MAX / stride) {
        return false;
    }
    command = *source;
    memcpy(colors, command.color_rgba, sizeof(colors));
    for (x = 0; x < width; ++x) {
        double horizontal = (double)x / (double)height;
        double centers[NK_UI_WAVES_RIBBON_COUNT];
        double half_widths[NK_UI_WAVES_RIBBON_COUNT];
        unsigned i;
        for (i = 0; i < NK_UI_WAVES_RIBBON_COUNT; ++i) {
            double wave = 0.0;
            double depth_scale = 0.6 + 0.4 * g_ribbon_depth[i];
            double width_breath;
            unsigned j;
            for (j = 0; j < 3u; ++j) {
                wave += g_wave_amplitude[j] *
                        sin(g_tau * g_wave_frequency[j] * horizontal +
                            command.phase[i][j]);
            }
            centers[i] = g_ribbon_base[i] + depth_scale * wave;
            width_breath = 1.0 + 0.25 *
                sin(g_tau * 0.23 * horizontal + command.width_phase[i]);
            half_widths[i] = g_ribbon_width[i] * width_breath;
        }
        for (y = 0; y < height; ++y) {
            float composite[3] = {0.0f, 0.0f, 0.0f};
            float alpha = 0.0f;
            double vertical = (double)y / (double)height;
            uint8_t *pixel = pixels + (size_t)y * stride + (size_t)x * 4u;
            for (i = 0; i < NK_UI_WAVES_RIBBON_COUNT; ++i) {
                double distance = (vertical - centers[i]) / half_widths[i];
                double absolute_distance = fabs(distance);
                double body = 1.0 - (double)nk_ui_smoothstep((float)absolute_distance);
                double peak = 0.12 + 0.07 * g_ribbon_depth[i];
                float ribbon_alpha = (float)(peak *
                    (0.45 * body * body + 0.55 * exp(-30.0 * distance * distance)));
                unsigned channel;
                for (channel = 0; channel < 3u; ++channel) {
                    composite[channel] = composite[channel] * (1.0f - ribbon_alpha) +
                                         colors[i][channel] * ribbon_alpha;
                }
                alpha = alpha * (1.0f - ribbon_alpha) + ribbon_alpha;
            }
            if (alpha > command.cap && alpha > 0.0f) {
                float reduction = command.cap / alpha;
                composite[0] *= reduction;
                composite[1] *= reduction;
                composite[2] *= reduction;
                alpha = command.cap;
            }
            pixel[0] = nk_ui_to_byte(composite[0]);
            pixel[1] = nk_ui_to_byte(composite[1]);
            pixel[2] = nk_ui_to_byte(composite[2]);
            pixel[3] = nk_ui_to_byte(alpha);
        }
    }
    return true;
}
