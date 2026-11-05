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
/*
 * fsr3_jitter.h - FSR 3.1.6's sub-pixel jitter: the phase count, the Halton(2, 3) offsets and the
 * context's ramped phase-count constant, as ffx_fsr3upscaler.cpp computes them (not as the
 * documentation describes them: the count is truncated, not rounded up). Pure C, no Vulkan, so the
 * replay's vertex stage and fsr3_pass.c share one sequence and a unit test pins it.
 *
 * Offsets are in render pixels, each component in [-0.5, 0.5). Content moves by +offset pixels with
 * y down; fsr3_pass.c passes the same pair to the upscaler as its jitter offset.
 */
#ifndef SR_FSR3_JITTER_H
#define SR_FSR3_JITTER_H

#include <stdint.h>

/* ffxFsr3UpscalerGetJitterPhaseCount: int32_t(8 * (display_width / render_width)^2), truncated. */
int32_t fsr3_jitter_phase_count(int32_t render_width, int32_t display_width);

/* AMD's halton(index, base), in float, as the SDK computes it. */
float fsr3_halton(int32_t index, int32_t base);

/* ffxFsr3UpscalerGetJitterOffset: halton((index % phase_count) + 1, 2 and 3) - 0.5. Returns 0, or
 * -1 (outputs untouched) when phase_count is not positive or index is negative. */
int fsr3_jitter_offset(float *x, float *y, int32_t index, int32_t phase_count);

/* The context's jitterPhaseCount constant for the next dispatch: the target on a reset or first
 * use (current == 0), otherwise one step of 1 towards it (ffx_fsr3upscaler.cpp, fsr3upscalerDispatch). */
float fsr3_jitter_ramp(float current, int32_t target, int reset);

/* The caller's sequence: the input counter restarts at 0 on a reset. */
typedef struct {
    int32_t index;
} Fsr3Jitter;

/* The offset for the next FSR 3 input, then advances the counter. -1 for sizes that give no phase. */
int fsr3_jitter_next(Fsr3Jitter *j, int reset, int32_t render_width, int32_t display_width,
                     float *x, float *y);

#endif /* SR_FSR3_JITTER_H */
