// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* PSP-side VFPU NaN/Inf matrix/multiply probe (issue #40).
 *
 * Settles the observable result-bit cells left unresolved by the host-synthetic
 * audit: for vmmul.t / vtfm3.t lanes whose dot product encounters NaNs,
 * infinity, or subnormals, the PSP's own output word (payload, quiet bit, and
 * sign) is printed as compact hex.  The inputs are the reduced deterministic
 * vector set from the issue; no retail/private data is required.
 *
 * Bit-transfer discipline (same as the transcendental oracle probe): inputs
 * are moved into the VFPU with mtv and results are moved out with mfv, so a
 * NaN payload or a denormal reaches and leaves the FPU without a host-side
 * float round trip.
 *
 * Run on hardware (or PPSSPP as corroboration only) and compare each
 * NAKAGAWA_PSP_TEST record against the host-runtime analysis in the issue.
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfpu_nan_cases.h"

PSP_MODULE_INFO("NAKAGAWA_VFPU_NAN", 0, 1, 0);
/* THREAD_ATTR_VFPU is mandatory: without it the first VFPU access traps. */
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define FIXTURE_BUILD_ID "nakagawa-vfpu-nan-v1"

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

/* One mtv per raw word: bit-exact moves into the t-size matrix/vector slots.
 * M000 -> S000,S001,S002 | S004,S005,S006 | S008,S009,S010
 * M100 -> S016,S017,S018 | S020,S021,S022 | S024,S025,S026
 * T000 -> S000,S001,S002 (vtfm3 vector operand) */
static void load_matrix_0(const unsigned int in[9]) {
    __asm__ volatile(
        "mtv %0, S000\n" "mtv %1, S001\n" "mtv %2, S002\n"
        "mtv %3, S004\n" "mtv %4, S005\n" "mtv %5, S006\n"
        "mtv %6, S008\n" "mtv %7, S009\n" "mtv %8, S010\n"
        :: "r"(in[0]), "r"(in[1]), "r"(in[2]), "r"(in[3]),
           "r"(in[4]), "r"(in[5]), "r"(in[6]), "r"(in[7]), "r"(in[8])
        : "memory");
}

static void load_matrix_1(const unsigned int in[9]) {
    __asm__ volatile(
        "mtv %0, S016\n" "mtv %1, S017\n" "mtv %2, S018\n"
        "mtv %3, S020\n" "mtv %4, S021\n" "mtv %5, S022\n"
        "mtv %6, S024\n" "mtv %7, S025\n" "mtv %8, S026\n"
        :: "r"(in[0]), "r"(in[1]), "r"(in[2]), "r"(in[3]),
           "r"(in[4]), "r"(in[5]), "r"(in[6]), "r"(in[7]), "r"(in[8])
        : "memory");
}

/* vmmul.t M200, M000, M100; result M200 -> S032,S033,S034 | S036,S037,S038 |
 * S040,S041,S042.  All nine lanes are read out; the divergent cell is (0,0). */
static void run_vmmul(unsigned int out[9]) {
    __asm__ volatile(
        "vmmul.t M200, M000, M100\n"
        "mfv %0, S032\n" "mfv %1, S033\n" "mfv %2, S034\n"
        "mfv %3, S036\n" "mfv %4, S037\n" "mfv %5, S038\n"
        "mfv %6, S040\n" "mfv %7, S041\n" "mfv %8, S042\n"
        : "=r"(out[0]), "=r"(out[1]), "=r"(out[2]), "=r"(out[3]),
          "=r"(out[4]), "=r"(out[5]), "=r"(out[6]), "=r"(out[7]), "=r"(out[8])
        :: "memory");
}

/* vtfm3.t T100, M100, T000; T000 = S000..S002 (first three words of the
 * case's M000 slot), result T100 -> S003,S004,S005. */
static void run_vtfm3(unsigned int out[3]) {
    __asm__ volatile(
        "vtfm3.t T100, M100, T000\n"
        "mfv %0, S003\n" "mfv %1, S004\n" "mfv %2, S005\n"
        : "=r"(out[0]), "=r"(out[1]), "=r"(out[2])
        :: "memory");
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const int emulated = emulator_present();
    char line[512];

    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_META schema=1 source=%s model=unknown firmware=unknown "
             "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
             "source_commit=0000000000000000000000000000000000000000 fixture=%s\n",
             emulated ? "ppsspp" : "psp", FIXTURE_BUILD_ID);
    emit(emulated, line);

    for (unsigned c = 0; c < VFPU_NAN_CASE_COUNT; ++c) {
        const VfpuNanCase *kase = &VFPU_NAN_CASES[c];
        load_matrix_0(&kase->in[0]);
        load_matrix_1(&kase->in[9]);

        if (kase->op == 0) {
            unsigned int out[9];
            run_vmmul(out);
            snprintf(line, sizeof(line),
                     "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-VFPU-002 case_id=%s "
                     "op=vmmul.t status=PASS "
                     "in=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                     "out=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x\n",
                     kase->id,
                     kase->in[0], kase->in[1], kase->in[2], kase->in[3], kase->in[4],
                     kase->in[5], kase->in[6], kase->in[7], kase->in[8],
                     kase->in[9], kase->in[10], kase->in[11], kase->in[12], kase->in[13],
                     kase->in[14], kase->in[15], kase->in[16], kase->in[17],
                     out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7], out[8]);
        } else {
            unsigned int out[3];
            run_vtfm3(out);
            snprintf(line, sizeof(line),
                     "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-VFPU-002 case_id=%s "
                     "op=vtfm3.t status=PASS "
                     "in=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                     "out=%08x,%08x,%08x\n",
                     kase->id,
                     kase->in[0], kase->in[1], kase->in[2], kase->in[3], kase->in[4],
                     kase->in[5], kase->in[6], kase->in[7], kase->in[8],
                     kase->in[9], kase->in[10], kase->in[11], kase->in[12], kase->in[13],
                     kase->in[14], kase->in[15], kase->in[16], kase->in[17],
                     out[0], out[1], out[2]);
        }
        emit(emulated, line);
    }

    /* Return rather than sceKernelExitGame (PSPLINK session teardown). */
    return 0;
}
