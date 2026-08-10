// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* PSP-side VFPU transcendental oracle (Loop A of docs/HARDWARE_ORACLE.md).
 *
 * Executes the eight VFPU transcendental instructions on real Allegrex silicon
 * over a fixed shared input vector and emits one scalar record per operation.
 *
 * Why this exists: assets/vfpu/ is PPSSPP's reconstruction of these tables
 * (assets/vfpu/PROVENANCE.json), and the existing vfpu_fuzz harness compares
 * generated code against sr_vfpu_interp -- both of which read those same
 * tables. That comparison agrees by construction and cannot detect a table
 * error. Only hardware can.
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfpu_oracle_cases.h"

PSP_MODULE_INFO("NAKAGAWA_VFPU_ORACLE", 0, 1, 0);
/* THREAD_ATTR_VFPU is mandatory. Without it the first VFPU access in this
   thread traps instead of executing, and the probe would report a fault rather
   than a table divergence. */
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

#define FIXTURE_BUILD_ID "nakagawa-vfpu-oracle-v1"

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

/* Each op moves the raw bits into S000, executes, and moves the raw result out.
 * mtv/mfv transfer bit patterns without an FPU round trip, so a NaN payload or
 * a denormal reaches the VFPU exactly as authored. */
#define VFPU_OP1(mnemonic)                                        \
    static uint32_t vfpu_##mnemonic(uint32_t bits) {              \
        uint32_t out;                                             \
        __asm__ volatile(                                         \
            "mtv %1, S000\n"                                      \
            #mnemonic ".s S001, S000\n"                           \
            "mfv %0, S001\n"                                      \
            : "=r"(out)                                           \
            : "r"(bits)                                           \
            : "memory");                                          \
        return out;                                               \
    }

VFPU_OP1(vrcp)
VFPU_OP1(vrsq)
VFPU_OP1(vsqrt)
VFPU_OP1(vasin)
VFPU_OP1(vlog2)
VFPU_OP1(vsin)
VFPU_OP1(vcos)
VFPU_OP1(vexp2)

typedef uint32_t (*vfpu_fn)(uint32_t);

typedef struct {
    const char *case_id;
    vfpu_fn     fn;
} VfpuCase;

static const VfpuCase CASES[] = {
    { "vfpu-vrcp",  vfpu_vrcp  },
    { "vfpu-vrsq",  vfpu_vrsq  },
    { "vfpu-vsqrt", vfpu_vsqrt },
    { "vfpu-vasin", vfpu_vasin },
    { "vfpu-vlog2", vfpu_vlog2 },
    { "vfpu-vsin",  vfpu_vsin  },
    { "vfpu-vcos",  vfpu_vcos  },
    { "vfpu-vexp2", vfpu_vexp2 },
};

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const int emulated = emulator_present();
    char line[320];

    snprintf(line, sizeof(line),
             "NAKAGAWA_PSP_META schema=1 source=%s model=unknown firmware=unknown "
             "binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 "
             "source_commit=0000000000000000000000000000000000000000 fixture=%s\n",
             emulated ? "ppsspp" : "psp", FIXTURE_BUILD_ID);
    emit(emulated, line);

    for (unsigned c = 0; c < sizeof(CASES) / sizeof(CASES[0]); ++c) {
        uint32_t digest = vfpu_oracle_digest_init();
        uint32_t spot[3] = { 0u, 0u, 0u };
        for (unsigned i = 0; i < VFPU_ORACLE_INPUT_COUNT; ++i) {
            const uint32_t r = CASES[c].fn(VFPU_ORACLE_INPUTS[i]);
            digest = vfpu_oracle_digest_step(digest, r);
            /* Spot values: 1.0, the smallest float above 1.0, and the smallest
               positive denormal -- the three most reconstruction-sensitive
               inputs in the vector. Indices track VFPU_ORACLE_INPUTS. */
            if (i == 2u)  spot[0] = r;
            if (i == 9u)  spot[1] = r;
            if (i == 12u) spot[2] = r;
        }
        /* status is PASS only in the sense that the op executed and produced a
           digest; correctness is decided by comparison, never self-reported. */
        snprintf(line, sizeof(line),
                 "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-VFPU-001 case_id=%s "
                 "status=PASS result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x\n",
                 CASES[c].case_id, (unsigned int)digest,
                 (unsigned int)spot[0], (unsigned int)spot[1], (unsigned int)spot[2],
                 (unsigned int)VFPU_ORACLE_INPUT_COUNT);
        emit(emulated, line);
    }

    /* Group B is emitted per-input for vsin/vcos only: upstream flags exactly
       this region as untested, and a digest would tell us THAT it diverged
       without telling us WHERE. Two ops x 16 inputs is still one launch. */
    for (unsigned c = 5; c <= 6; ++c) {
        for (unsigned i = VFPU_ORACLE_LARGE_ARG_FIRST; i < VFPU_ORACLE_INPUT_COUNT; ++i) {
            const uint32_t in  = VFPU_ORACLE_INPUTS[i];
            const uint32_t out = CASES[c].fn(in);
            snprintf(line, sizeof(line),
                     "NAKAGAWA_PSP_TEST schema=1 test_id=PSP-VFPU-001 case_id=%s-arg%02u "
                     "status=PASS result=0x%08x out0=0x%08x\n",
                     CASES[c].case_id, (unsigned int)i, (unsigned int)out, (unsigned int)in);
            emit(emulated, line);
        }
    }

    /* Return rather than calling sceKernelExitGame: PSPLINK's resetonexit
       controls session teardown, and the CRT emits the exit call anyway. */
    return 0;
}
