/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* PSP-side VFPU source/destination aliasing probe.
 *
 * Settles the overlap result-bit cells the host audit classifies as
 * hardware-unresolved (contract UNESTABLED) and provides silicon evidence for
 * the NO_OVERLAP cells (whose overlapping encodings the docs predict give
 * INCORRECT results on hardware) and the ALLOWED cells (whose snapshot
 * semantics the host implementations claim).  For every case the probe prints
 * the full 128-lane register file AFTER the instruction runs, as raw hex
 * words, so the oracle lane can diff hardware against the host analysis
 * without any host-side float round trip.
 *
 * Inputs: one deterministic integer fill of all 128 registers (finite,
 * distinct values with +0/-0/+inf/-inf/+NaN/-NaN lanes sprinkled in).  Raw
 * bit patterns only, never decimal float literals -- a decimal literal is
 * re-rounded by each compiler and would silently change the question asked.
 * The fill is reproduced in the probe source below so the host-side
 * simulation can be repeated bit-exactly.
 *
 * Bit-transfer discipline: registers are filled with lv.q and read out with
 * sv.q (raw word copies, same guarantee as mtv/mfv used by the NaN probe).
 * Prefix registers are set to identity (vpfxs/vpfxt 0xE4, vpfxd 0) before
 * each case.
 *
 * Record classes (see tools/vfpu_overlap_fuzz_gen.py and the vfpu-docs
 * register-hazard evidence): contract 0 = ALLOWED (disjoint or
 * docs-compatible overlap), 1 = NO_OVERLAP (docs say overlapping encodings
 * give incorrect results), 2 = UNESTABLED (no public hardware evidence).
 *
 * Run on hardware (or PPSSPP as corroboration only) and compare each
 * NAKAGAWA_PSP_TEST record against the host-runtime analysis.  The PPSSPP
 * route must be labelled "ppsspp" evidence, never hardware.
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfpu_overlap_cases.h"

PSP_MODULE_INFO("NAKAGAWA_VFPU_OVERLAP", 0, 1, 0);
/* THREAD_ATTR_VFPU is mandatory: without it the first VFPU access traps. */
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define FIXTURE_BUILD_ID "nakagawa-vfpu-overlap-v1"

#define EMULATOR_DEVCTL_SEND_OUTPUT 2
#define EMULATOR_DEVCTL_IS_EMULATOR 3

static int emulator_present(void) {
    uint32_t flag = 0;
    if (sceIoDevctl("emulator:", EMULATOR_DEVCTL_IS_EMULATOR, NULL, 0, &flag, sizeof(flag)) < 0) {
        return 0;
    }
    return flag == 1;
}

static void emit(int emulated, const char *text) {
    if (emulated) {
        sceIoDevctl("emulator:", EMULATOR_DEVCTL_SEND_OUTPUT, (void *)text, (int)strlen(text), NULL, 0);
    } else {
        printf("%s", text);
    }
}

/* ------------------------------------------------------------------ */
/* Deterministic 128-lane input fill                                  */
/* ------------------------------------------------------------------ */

/* Lane i fill: 0x3F000000 + i*0x00800000 spans the finite range
 * 0x3F000000..0x7E800000 (0.5 .. ~1.08e38) with every lane distinct.  Some
 * lanes are then overridden with special raw words (order matters, stays
 * deterministic): multiples of 13 -> +0.0, 17 -> -0.0, 19 -> +inf,
 * 23 -> -inf, 29 -> +NaN(payload 1), 31 -> -NaN.  i == 0 hits all of them
 * and ends as -NaN; the interplay is intentional (NaN propagation lanes). */
static void fill_inputs(unsigned int v[128]) {
    for (int i = 0; i < 128; i++) {
        v[i] = 0x3F000000u + (unsigned int)(i * 0x00800000u);
    }
    for (int i = 0; i < 128; i++) {
        if (i % 13 == 0) v[i] = 0x00000000u;
        if (i % 17 == 0) v[i] = 0x80000000u;
        if (i % 19 == 0) v[i] = 0x7F800000u;
        if (i % 23 == 0) v[i] = 0xFF800000u;
        if (i % 29 == 0) v[i] = 0x7FC00001u;
        if (i % 31 == 0) v[i] = 0xFFC00000u;
    }
}

/* ------------------------------------------------------------------ */
/* Full register-file load/save (raw word copies)                     */
/* ------------------------------------------------------------------ */

static void load_all_vfpu_regs(const unsigned int *src) {
    __asm__ volatile(
        "lv.q R000,   0(%0)\n" "lv.q R001,  16(%0)\n" "lv.q R002,  32(%0)\n" "lv.q R003,  48(%0)\n"
        "lv.q R010,  64(%0)\n" "lv.q R011,  80(%0)\n" "lv.q R012,  96(%0)\n" "lv.q R013, 112(%0)\n"
        "lv.q R020, 128(%0)\n" "lv.q R021, 144(%0)\n" "lv.q R022, 160(%0)\n" "lv.q R023, 176(%0)\n"
        "lv.q R030, 192(%0)\n" "lv.q R031, 208(%0)\n" "lv.q R032, 224(%0)\n" "lv.q R033, 240(%0)\n"
        "lv.q R040, 256(%0)\n" "lv.q R041, 272(%0)\n" "lv.q R042, 288(%0)\n" "lv.q R043, 304(%0)\n"
        "lv.q R050, 320(%0)\n" "lv.q R051, 336(%0)\n" "lv.q R052, 352(%0)\n" "lv.q R053, 368(%0)\n"
        "lv.q R060, 384(%0)\n" "lv.q R061, 400(%0)\n" "lv.q R062, 416(%0)\n" "lv.q R063, 432(%0)\n"
        "lv.q R070, 448(%0)\n" "lv.q R071, 464(%0)\n" "lv.q R072, 480(%0)\n" "lv.q R073, 496(%0)\n"
        :: "r"(src) : "memory");
}

static void save_all_vfpu_regs(unsigned int *dst) {
    __asm__ volatile(
        "sv.q R000,   0(%0)\n" "sv.q R001,  16(%0)\n" "sv.q R002,  32(%0)\n" "sv.q R003,  48(%0)\n"
        "sv.q R010,  64(%0)\n" "sv.q R011,  80(%0)\n" "sv.q R012,  96(%0)\n" "sv.q R013, 112(%0)\n"
        "sv.q R020, 128(%0)\n" "sv.q R021, 144(%0)\n" "sv.q R022, 160(%0)\n" "sv.q R023, 176(%0)\n"
        "sv.q R030, 192(%0)\n" "sv.q R031, 208(%0)\n" "sv.q R032, 224(%0)\n" "sv.q R033, 240(%0)\n"
        "sv.q R040, 256(%0)\n" "sv.q R041, 272(%0)\n" "sv.q R042, 288(%0)\n" "sv.q R043, 304(%0)\n"
        "sv.q R050, 320(%0)\n" "sv.q R051, 336(%0)\n" "sv.q R052, 352(%0)\n" "sv.q R053, 368(%0)\n"
        "sv.q R060, 384(%0)\n" "sv.q R061, 400(%0)\n" "sv.q R062, 416(%0)\n" "sv.q R063, 432(%0)\n"
        "sv.q R070, 448(%0)\n" "sv.q R071, 464(%0)\n" "sv.q R072, 480(%0)\n" "sv.q R073, 496(%0)\n"
        :: "r"(dst) : "memory");
}

/* Identity prefixes: S/T = [x,y,z,w] (0xE4), D = no-op (0). */
static void set_identity_prefixes(void) {
    __asm__ volatile(
        "mtvc %0, $128\n" /* VFPU_PFXS */
        "mtvc %1, $129\n" /* VFPU_PFXT */
        "mtvc %2, $130\n" /* VFPU_PFXD */
        :: "r"(0xE4u), "r"(0xE4u), "r"(0u) : "memory");
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const int emulated = emulator_present();
    char line[2048];

    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_META schema=1 source=%s model=unknown firmware=unknown "
             "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
             "source_commit=0000000000000000000000000000000000000000 fixture=%s\n",
             emulated ? "ppsspp" : "psp", FIXTURE_BUILD_ID);
    emit(emulated, line);

    static unsigned int fill[128] __attribute__((aligned(16)));
    static unsigned int out[128] __attribute__((aligned(16)));
    fill_inputs(fill);

    for (unsigned c = 0; c < VFPU_OVERLAP_PROBE_CASE_COUNT; ++c) {
        const VfpuOverlapProbeCase *kase = &VFPU_OVERLAP_PROBE_CASES[c];
        load_all_vfpu_regs(fill);
        set_identity_prefixes();
        __asm__ volatile(".word 0x%0" :: "i"(kase->w) : "memory");
        save_all_vfpu_regs(out);

        size_t pos = 0;
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos,
                                "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-VFPU-003 case_id=%s "
                                "op=%08x klass=%u family=%u contract=%u status=PASS out=",
                                kase->id, kase->w, kase->klass, kase->family, kase->contract);
        for (int i = 0; i < 128; i++) {
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "%s%08x",
                                    i ? "," : "", out[i]);
        }
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "\n");
        emit(emulated, line);
    }

    return 0;
}
