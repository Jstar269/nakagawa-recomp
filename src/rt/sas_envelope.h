/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (c) 2012- PPSSPP Project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.0 or later versions.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License 2.0 for more details.
 *
 * A copy of the GPL 2.0 should have been included with the program.
 * If not, see http://www.gnu.org/licenses/
 *
 * Official git repository and contact information can be found at
 * https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.
 *
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * sas_envelope.h: the sceSasCore voice envelope (ADSR) model, issue #1296.
 *
 * Adapted from PPSSPP (https://github.com/hrydgard/ppsspp) at
 * 3bc60270eae3069f13dfbfdb7292a7e1375de6f6, Core/HW/SasAudio.cpp and
 * Core/HW/SasAudio.h: class ADSREnvelope (SetSimpleEnvelope, SetEnvelope,
 * SetRate, WalkCurve, Step, KeyOn, KeyOff, End, GetHeight) and the
 * simple-envelope bitfield decoders (simpleRate, exponentRate, getAttackRate
 * and the rest); and Core/HLE/sceSas.cpp: the per-phase curve check of
 * sceSasSetADSRMode. PPSSPP's comment cites JPCSP's sceSasCore.java for the
 * bitfield layout. Label: derived-translated
 * (docs/provenance/INDEPENDENCE_MODEL.md).
 *
 * Modified for this project (2026-11-04): translated from C++ to C99 static
 * inline functions over a plain struct; the per-title RockmanDash2SoundFix
 * compatibility flag is left out; savestate (DoState) and debug-string code
 * are left out; KeyOn enters the attack state directly, without PPSSPP's
 * STATE_KEYON / STATE_KEYON_STEP delay, because that delay belongs with the
 * mixer's key-on sample delay (the first 32 output samples after a key-on,
 * SasAudio.cpp MixVoice), which this runtime does not model yet; a voice that
 * no envelope call has configured keeps this runtime's previous default curve
 * (sas_envelope_reset). That default departs from both references: PPSSPP
 * (SasAudio.h) and jpcsp (SoundVoice.java) start every rate and the sustain
 * level at 0, so an unconfigured voice stays silent there. It is kept until a
 * PSP measurement settles it. The height read back is also clamped at 0 from
 * below (PPSSPP clamps only the top), so a decay step that overshoots zero
 * never reads as a negative height.
 * The rate formulas and curves are PPSSPP's model, not PSP measurements.
 */
#ifndef NK_RT_SAS_ENVELOPE_H
#define NK_RT_SAS_ENVELOPE_H

#include <stdint.h>

#define SAS_ENVELOPE_HEIGHT_MAX 0x40000000

/* Curve modes, as __sceSasSetADSRmode numbers them. */
enum {
    SAS_CURVE_LINEAR_INCREASE = 0,
    SAS_CURVE_LINEAR_DECREASE = 1,
    SAS_CURVE_LINEAR_BENT = 2,
    SAS_CURVE_EXPONENT_DECREASE = 3,
    SAS_CURVE_EXPONENT_INCREASE = 4,
    SAS_CURVE_DIRECT = 5,
};

/* Envelope states. ATTACK..RELEASE are the PSP's phase numbers. */
enum {
    SAS_ENV_OFF = -1,
    SAS_ENV_ATTACK = 0,
    SAS_ENV_DECAY = 1,
    SAS_ENV_SUSTAIN = 2,
    SAS_ENV_RELEASE = 3,
};

typedef struct {
    int attack_rate, decay_rate, sustain_rate, release_rate;
    int attack_type, decay_type, sustain_type, release_type;
    int sustain_level;
    int state;
    int64_t height;   /* 64-bit so a step never has to care about overflow */
} SasEnvelope;

/* The curve a voice has before any envelope call. This keeps the runtime's earlier
 * behaviour for such voices (attack to full height in 64 samples, held while keyed on,
 * release in 64 samples); what a PSP does for a voice the game never configures is
 * unmeasured. */
static inline void sas_envelope_reset(SasEnvelope *e) {
    e->attack_rate = 0x1000000;  e->attack_type = SAS_CURVE_LINEAR_INCREASE;
    e->decay_rate = 0;           e->decay_type = SAS_CURVE_LINEAR_DECREASE;
    e->sustain_rate = 0;         e->sustain_type = SAS_CURVE_LINEAR_DECREASE;
    e->release_rate = 0x1000000; e->release_type = SAS_CURVE_LINEAR_DECREASE;
    e->sustain_level = SAS_ENVELOPE_HEIGHT_MAX;
    e->state = SAS_ENV_OFF;
    e->height = 0;
}

static inline int sas_envelope_simple_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int rate = (int)(((uint32_t)(7 - (n & 0x3)) << 26) >> (n >> 2));
    return rate == 0 ? 1 : rate;
}

static inline int sas_envelope_exponent_rate(int n) {
    n &= 0x7F;
    if (n == 0x7F) return 0;
    int rate = (int)(((uint32_t)(7 - (n & 0x3)) << 24) >> (n >> 2));
    return rate == 0 ? 1 : rate;
}

/* __sceSasSetSimpleADSR: two packed 16-bit words.
 *   env1: bit 15 attack curve (0 linear increase, 1 bent), bits 8-14 attack rate,
 *         bits 4-7 decay rate (exponential decrease), bits 0-3 sustain level.
 *   env2: bits 14-15 sustain curve, bits 6-12 sustain rate, bit 5 release curve
 *         (0 linear decrease, 1 exponential decrease), bits 0-4 release rate.
 * The caller has already refused env2 with bit 13 set. */
static inline void sas_envelope_set_simple(SasEnvelope *e, uint32_t env1, uint32_t env2) {
    env1 &= 0xFFFFu;
    env2 &= 0xFFFFu;
    e->attack_rate = sas_envelope_simple_rate((int)(env1 >> 8));
    e->attack_type = (env1 & 0x8000u) ? SAS_CURVE_LINEAR_BENT : SAS_CURVE_LINEAR_INCREASE;

    int dn = (int)((env1 >> 4) & 0xFu);
    e->decay_rate = dn == 0 ? 0x7FFFFFFF : (int)(0x80000000u >> dn);
    e->decay_type = SAS_CURVE_EXPONENT_DECREASE;

    e->sustain_type = (int)((env2 >> 14) & 3u);
    e->sustain_rate = e->sustain_type == SAS_CURVE_EXPONENT_DECREASE
        ? sas_envelope_exponent_rate((int)(env2 >> 6))
        : sas_envelope_simple_rate((int)(env2 >> 6));

    int rn = (int)(env2 & 0x1Fu);
    e->release_type = (env2 & 0x20u) ? SAS_CURVE_EXPONENT_DECREASE : SAS_CURVE_LINEAR_DECREASE;
    if (rn == 31) {
        e->release_rate = 0;
    } else if (e->release_type == SAS_CURVE_LINEAR_DECREASE) {
        e->release_rate = rn == 30 ? 0x40000000 : rn == 29 ? 1 : (int)(0x10000000u >> rn);
    } else {
        e->release_rate = rn == 0 ? 0x7FFFFFFF : (int)(0x80000000u >> rn);
    }

    e->sustain_level = (int)(((env1 & 0xFu) + 1u) << 26);
}

/* __sceSasSetADSRmode's per-phase check (PPSSPP sceSas.cpp sceSasSetADSRMode, as jpcsp's
 * checkADSRmode): the attack takes the increasing curves 0, 2 and 4, the decay and the release
 * the decreasing ones 1, 3 and 5, the sustain any of 0..5. The caller has cleared each value's
 * top bit, which the PSP ignores. Returns the phase bits (1 attack, 2 decay, 4 sustain,
 * 8 release) whose curve is refused; the call fails when one of them is in its mask. */
static inline uint32_t sas_envelope_refused_curves(int a, int d, int s, int r) {
    uint32_t refused = 0;
    if (a < 0 || a > 5 || (a & 1) != 0) refused |= 0x1u;
    if (d < 0 || d > 5 || (d & 1) != 1) refused |= 0x2u;
    if (s < 0 || s > 5) refused |= 0x4u;
    if (r < 0 || r > 5 || (r & 1) != 1) refused |= 0x8u;
    return refused;
}

/* __sceSasSetADSRmode: the selected phases' curves (checked by sas_envelope_refused_curves). */
static inline void sas_envelope_set_curves(SasEnvelope *e, uint32_t mask, int a, int d, int s, int r) {
    if (mask & 0x1u) e->attack_type = a;
    if (mask & 0x2u) e->decay_type = d;
    if (mask & 0x4u) e->sustain_type = s;
    if (mask & 0x8u) e->release_type = r;
}

/* __sceSasSetADSR: the selected phases' rates (non-negative, validated by the caller). */
static inline void sas_envelope_set_rates(SasEnvelope *e, uint32_t mask, int a, int d, int s, int r) {
    if (mask & 0x1u) e->attack_rate = a;
    if (mask & 0x2u) e->decay_rate = d;
    if (mask & 0x4u) e->sustain_rate = s;
    if (mask & 0x8u) e->release_rate = r;
}

static inline void sas_envelope_set_sustain_level(SasEnvelope *e, int level) {
    e->sustain_level = level;
}

static inline void sas_envelope_walk(SasEnvelope *e, int type, int rate) {
    int64_t exp_delta;
    switch (type) {
    case SAS_CURVE_LINEAR_INCREASE:
        e->height += rate;
        break;
    case SAS_CURVE_LINEAR_DECREASE:
        e->height -= rate;
        break;
    case SAS_CURVE_LINEAR_BENT:
        if (e->height <= (int64_t)SAS_ENVELOPE_HEIGHT_MAX * 3 / 4) e->height += rate;
        else e->height += rate / 4;
        break;
    case SAS_CURVE_EXPONENT_DECREASE:
        exp_delta = e->height - SAS_ENVELOPE_HEIGHT_MAX;
        exp_delta += (-exp_delta * rate) >> 32;
        e->height = exp_delta + SAS_ENVELOPE_HEIGHT_MAX - ((int64_t)(uint32_t)rate + 3) / 4;
        break;
    case SAS_CURVE_EXPONENT_INCREASE:
        exp_delta = e->height - SAS_ENVELOPE_HEIGHT_MAX;
        exp_delta += (-exp_delta * rate) >> 32;
        e->height = exp_delta + 0x4000 + SAS_ENVELOPE_HEIGHT_MAX;
        break;
    case SAS_CURVE_DIRECT:
        e->height = rate;
        break;
    default:
        break;
    }
}

static inline void sas_envelope_set_state(SasEnvelope *e, int state) {
    if (e->height > SAS_ENVELOPE_HEIGHT_MAX) e->height = SAS_ENVELOPE_HEIGHT_MAX;
    e->state = state;
}

/* One output sample's step. */
static inline void sas_envelope_step(SasEnvelope *e) {
    switch (e->state) {
    case SAS_ENV_ATTACK:
        sas_envelope_walk(e, e->attack_type, e->attack_rate);
        if (e->height >= SAS_ENVELOPE_HEIGHT_MAX || e->height < 0) sas_envelope_set_state(e, SAS_ENV_DECAY);
        break;
    case SAS_ENV_DECAY:
        sas_envelope_walk(e, e->decay_type, e->decay_rate);
        if (e->height < e->sustain_level) sas_envelope_set_state(e, SAS_ENV_SUSTAIN);
        break;
    case SAS_ENV_SUSTAIN:
        sas_envelope_walk(e, e->sustain_type, e->sustain_rate);
        if (e->height <= 0) { e->height = 0; sas_envelope_set_state(e, SAS_ENV_RELEASE); }
        break;
    case SAS_ENV_RELEASE:
        sas_envelope_walk(e, e->release_type, e->release_rate);
        if (e->height <= 0) { e->height = 0; sas_envelope_set_state(e, SAS_ENV_OFF); }
        break;
    default:
        break;
    }
}

static inline void sas_envelope_key_on(SasEnvelope *e) {
    e->height = 0;
    sas_envelope_set_state(e, SAS_ENV_ATTACK);
}

static inline void sas_envelope_key_off(SasEnvelope *e) {
    sas_envelope_set_state(e, SAS_ENV_RELEASE);
}

static inline void sas_envelope_end(SasEnvelope *e) {
    sas_envelope_set_state(e, SAS_ENV_OFF);
    e->height = 0;
}

/* The height a game reads back (__sceSasGetEnvelopeHeight), 0..SAS_ENVELOPE_HEIGHT_MAX. */
static inline int sas_envelope_height(const SasEnvelope *e) {
    if (e->height > SAS_ENVELOPE_HEIGHT_MAX) return SAS_ENVELOPE_HEIGHT_MAX;
    if (e->height < 0) return 0;
    return (int)e->height;
}

static inline int sas_envelope_ended(const SasEnvelope *e) {
    return e->state == SAS_ENV_OFF;
}

#endif /* NK_RT_SAS_ENVELOPE_H */
