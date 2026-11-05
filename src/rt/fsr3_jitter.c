/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * Derived from the AMD FidelityFX SDK v2.4.0 (commit 7539ae8b6568ad29ed78a753404351daf3b5cca7),
 * Kits/FidelityFX/upscalers/fsr3/internal/ffx_fsr3upscaler.cpp, under the MIT licence below.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files(the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions :
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Modified by Nakagawa Recomp contributors, 2026-11-05: translated from AMD's C++ host to C.
 */
/* fsr3_jitter.c - see fsr3_jitter.h. */
#include "fsr3_jitter.h"

#include <math.h>

int32_t fsr3_jitter_phase_count(int32_t render_width, int32_t display_width) {
    /* AMD: int32_t(basePhaseCount * pow(float(displayWidth) / renderWidth, 2.0f)). The SDK's
     * reference build (MSVC, <cmath>) takes the float overload of pow; powf is that overload. */
    if (render_width <= 0 || display_width <= 0) return 0;
    const float base_phase_count = 8.0f;
    return (int32_t)(base_phase_count * powf((float)display_width / (float)render_width, 2.0f));
}

float fsr3_halton(int32_t index, int32_t base) {
    float f = 1.0f, result = 0.0f;
    for (int32_t current = index; current > 0;) {
        f /= (float)base;
        result = result + f * (float)(current % base);
        current = (int32_t)(uint32_t)(floorf((float)current / (float)base));
    }
    return result;
}

int fsr3_jitter_offset(float *x, float *y, int32_t index, int32_t phase_count) {
    if (!x || !y || phase_count <= 0) return -1;
    *x = fsr3_halton((index % phase_count) + 1, 2) - 0.5f;
    *y = fsr3_halton((index % phase_count) + 1, 3) - 0.5f;
    return 0;
}

float fsr3_jitter_ramp(float current, int32_t target, int reset) {
    if (reset || current == 0.0f) return (float)target;
    const int32_t delta = (int32_t)((float)target - current);
    if (delta > 0) return current + 1.0f;
    if (delta < 0) return current - 1.0f;
    return current;
}

int fsr3_jitter_next(Fsr3Jitter *j, int reset, int32_t render_width, int32_t display_width,
                     float *x, float *y) {
    if (!j) return -1;
    if (reset) j->index = 0;
    const int32_t n = fsr3_jitter_phase_count(render_width, display_width);
    if (fsr3_jitter_offset(x, y, j->index, n) != 0) return -1;
    /* AMD's caller passes an ever-growing index and the SDK takes it modulo the count, so a change of
     * count continues from the same index. It restarts at 0 only where int32_t would overflow. */
    j->index = j->index == INT32_MAX ? 0 : j->index + 1;
    return 0;
}

void fsr3_jitter_clip_offset(float jx, float jy, float viewport_w, float viewport_h, float out[2]) {
    out[0] = 2.0f * jx / viewport_w;
    out[1] = 2.0f * jy / viewport_h;
}
