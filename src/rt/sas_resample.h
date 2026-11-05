/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * sas_resample.h - band-limited resampling of one SAS voice's decoded stream (Ace Mode's sas=sinc,
 * #2054, docs/ENHANCEMENT_CONTRACT.md section 10.7). Header-only and all static: src/rt/hle_audio.c
 * mixes with it, and the hle-thread selftest includes it as the reference renderer of its model.
 *
 * The caller gives the decoded stream around the voice and, for each output sample, the stream
 * index the authentic mixer played and its 12-bit fraction. This file turns them into sample
 * values with a Kaiser-windowed sinc evaluated at the exact position (index + fraction / 4096).
 * Above a pitch of 0x1000 the kernel widens by the pitch ratio, so the cutoff falls below the
 * output's Nyquist frequency and the decimation does not alias. It reads nothing else and
 * changes nothing the guest can see: the caller keeps every piece of voice state.
 *
 * C library and libm only.
 */
#ifndef SR_SAS_RESAMPLE_H
#define SR_SAS_RESAMPLE_H

#include <math.h>
#include <stdint.h>

#define SAS_RS_ZEROS 24              /* zero crossings each side at unity pitch */
#define SAS_RS_STEPS 128             /* kernel table entries per zero crossing */
#define SAS_RS_BETA 7.0              /* Kaiser window: about 70 dB stopband */
#define SAS_RS_CUTOFF 0.91           /* cutoff as a share of the lower Nyquist frequency */
#define SAS_RS_PITCH_MAX 0x4000      /* the SAS core's highest pitch (4 source samples per output) */
/* The most source samples the widest kernel reads on one side of a position:
 * SAS_RS_ZEROS * (SAS_RS_PITCH_MAX / 0x1000) / SAS_RS_CUTOFF, rounded up. */
#define SAS_RS_REACH 108

static float s_sas_rs_kernel[SAS_RS_ZEROS * SAS_RS_STEPS + 2];
static int s_sas_rs_kernel_ready;

static double sas_rs_bessel_i0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64 && term > sum * 1e-17; k++) {
        term *= q / ((double)k * (double)k);
        sum += term;
    }
    return sum;
}

static void sas_rs_kernel_init(void) {
    const double pi = 3.14159265358979323846;
    if (s_sas_rs_kernel_ready) return;
    double norm = sas_rs_bessel_i0(SAS_RS_BETA);
    for (int j = 0; j < SAS_RS_ZEROS * SAS_RS_STEPS; j++) {
        double x = (double)j / SAS_RS_STEPS, r = x / SAS_RS_ZEROS;
        double sinc = j ? sin(pi * x) / (pi * x) : 1.0;
        s_sas_rs_kernel[j] = (float)(sinc * sas_rs_bessel_i0(SAS_RS_BETA * sqrt(1.0 - r * r)) / norm);
    }
    s_sas_rs_kernel[SAS_RS_ZEROS * SAS_RS_STEPS] = 0.0f;
    s_sas_rs_kernel[SAS_RS_ZEROS * SAS_RS_STEPS + 1] = 0.0f;
    s_sas_rs_kernel_ready = 1;
}

/* The cutoff as a share of the source's Nyquist frequency: SAS_RS_CUTOFF, lowered by the pitch
 * ratio above 0x1000 so it sits below the output's Nyquist frequency. */
static double sas_rs_cutoff(int pitch) {
    if (pitch <= 0) pitch = 0x1000;
    return SAS_RS_CUTOFF * (pitch > 0x1000 ? 4096.0 / pitch : 1.0);
}

/* How many source samples the kernel reaches on one side of a position at this pitch, rounded
 * up: from 27 at 0x1000 and below to 106 at 0x4000 (never above SAS_RS_REACH). An output sample at
 * stream position k + frac reads no source sample past k + sas_rs_reach(pitch). */
static int32_t sas_rs_reach(int pitch) {
    return (int32_t)ceil(SAS_RS_ZEROS / sas_rs_cutoff(pitch));
}

/* Writes count samples to out. src[j] is stream index j for -SAS_RS_REACH <= j < src_end; k[i]
 * and frac[i] (0..0xFFF) are output sample i's position. pitch is the voice's (0x1000 = 1). */
static void sas_rs_render(const int16_t *src, int32_t src_end, const int32_t *k, const uint16_t *frac,
                          int count, int pitch, int16_t *out) {
    sas_rs_kernel_init();
    double fc = sas_rs_cutoff(pitch);
    double reach = SAS_RS_ZEROS / fc, scale = fc * SAS_RS_STEPS;
    for (int i = 0; i < count; i++) {
        double p = (double)k[i] + (double)frac[i] / 4096.0;
        int32_t lo = (int32_t)ceil(p - reach), hi = (int32_t)floor(p + reach);
        if (lo < -SAS_RS_REACH) lo = -SAS_RS_REACH;
        if (hi > src_end - 1) hi = src_end - 1;
        double acc = 0.0, wsum = 0.0;
        for (int32_t j = lo; j <= hi; j++) {
            double x = fabs(p - (double)j) * scale;
            int xi = (int)x;
            if (xi >= SAS_RS_ZEROS * SAS_RS_STEPS) continue;
            double w = s_sas_rs_kernel[xi] + (s_sas_rs_kernel[xi + 1] - s_sas_rs_kernel[xi]) * (x - xi);
            acc += w * src[j];
            wsum += w;
        }
        /* Dividing by the weights' sum keeps a constant signal exact at every fraction. */
        double y = wsum > 0.0 ? acc / wsum : 0.0;
        double r = floor(y + 0.5);
        out[i] = (int16_t)(r > 32767.0 ? 32767 : r < -32768.0 ? -32768 : (int)r);
    }
}

#endif /* SR_SAS_RESAMPLE_H */
