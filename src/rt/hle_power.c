// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

#include "hle_power.h"

#define A0 (s->r[4])
#define A1 (s->r[5])
#define A2 (s->r[6])

/* scePower */
uint32_t h_PowerGetBatteryLifePercent(CpuState *s) { (void)s; return 100; }
uint32_t h_PowerIsBatteryCharging(CpuState *s) { (void)s; return 1; }
uint32_t h_PowerIsBatteryExist(CpuState *s) { (void)s; return 1; }
uint32_t h_PowerIsPowerOnline(CpuState *s) { (void)s; return 1; }

/* scePowerIsLowBattery(void): check whether the battery is low (PSPSDK psppower.h). That header gives
 * no return value, so the encoding is UNMEASURED; this model reports 0 (not low) while the modeled
 * battery is full, which h_PowerGetBatteryLifePercent reports as 100. The flag is a model. */
static int s_power_battery_low = 0;
uint32_t h_PowerIsLowBattery(CpuState *s) { (void)s; return s_power_battery_low ? 1u : 0u; }

#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_power_set_low_battery(int low) { s_power_battery_low = low != 0; }
#endif

/* Retained clock request in MHz. Public behaviour reference: PSPSDK
 * src/power/psppower.h (scePowerSetClockFrequency) and PPSSPP
 * Core/HLE/scePower.cpp, where the Set calls update the frequencies the Get
 * calls report. The getters below previously returned fixed 333/166; they now
 * reflect the last accepted Set request. Frequency validation and any 350 MHz
 * ceiling difference between the two Set variants are UNMEASURED here: every
 * request is retained verbatim and reported back.
 *
 * PLL: the PSPSDK header documents pllfreq as valid from 19 to 333 MHz, with
 * cpufreq <= pllfreq and busfreq * 2 <= pllfreq. It documents no power-on value.
 * Measured on the PSP-3000 oracle (2026-10-10, hle-power-clock) before any Set:
 * PLL 222, CPU 222, bus 111 MHz, from the Int getters and the Float getters
 * (222.0f and 111.0f). A title that wants 333 asks for it. */
static uint32_t s_pll_freq = 222u, s_cpu_freq = 222u, s_bus_freq = 111u;

uint32_t h_PowerGetPllClockFrequencyInt(CpuState *s) { (void)s; return s_pll_freq; }
uint32_t h_PowerGetCpuClockFrequencyInt(CpuState *s) { (void)s; return s_cpu_freq; }
uint32_t h_PowerGetBusClockFrequencyInt(CpuState *s) { (void)s; return s_bus_freq; }

/* Float getters: the single-precision value of the same retained clock the Int getters report, written
 * to $f0 (the MIPS float-return convention). PSP-3000 6.61 oracle 2026-10-15, record PSP-HLE-POWER-001
 * (cases pll-clock-float, cpu-clock-float, bus-clock-float): the defaults read back as 0x435E0000
 * (222.0) and 0x42DE0000 (111.0). The raw-register run PSP-HLE-POWER-002 (2026-10-15, build b5d6fc5b)
 * read the same $f0 bits. It also read $v0 after each call: neither 0 nor the clock, a different
 * user-memory-range word each time, and the probe did not preset $v0, so it does not show whether a
 * getter writes $v0 at all. That value is not modelled. The 0 these handlers return into $v0 is this
 * runtime's choice, not a PSP value; a caller that reads the result as a float does not see it. */
uint32_t h_PowerGetPllClockFrequencyFloat(CpuState *s) { s->f[0] = (float)s_pll_freq; return 0; }
uint32_t h_PowerGetCpuClockFrequencyFloat(CpuState *s) { s->f[0] = (float)s_cpu_freq; return 0; }
uint32_t h_PowerGetBusClockFrequencyFloat(CpuState *s) { s->f[0] = (float)s_bus_freq; return 0; }

/* scePowerSetClockFrequency(pllfreq, cpufreq, busfreq): retain the requested
 * PLL, CPU and bus clocks. Same public references as the retained state above. */
uint32_t h_PowerSetClockFrequency(CpuState *s) {
    s_pll_freq = A0;
    s_cpu_freq = A1;
    s_bus_freq = A2;
    return 0;
}

/* scePowerSetClockFrequency350(pllfreq, cpufreq, busfreq): same shape, same
 * retained state. Whether firmware 350 permits a distinct PLL ceiling is
 * UNMEASURED here. */
uint32_t h_PowerSetClockFrequency350(CpuState *s) {
    s_pll_freq = A0;
    s_cpu_freq = A1;
    s_bus_freq = A2;
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only reset so the executable harness can isolate the retained
 * clock fixtures from one another. Adds no production behaviour. */
void sr_hle_test_power_reset(void) { s_pll_freq = 222u; s_cpu_freq = 222u; s_bus_freq = 111u; }
#endif

static uint32_t s_power_cb_slots[16];

uint32_t h_PowerRegisterCallback(CpuState *s) {
    int32_t slot = (int32_t)A0;
    uint32_t cb_uid = A1;

    if (slot < -1 || slot >= 32) return 0x80000102u;
    if (slot >= 16) return 0x80000023u;
    if (!sr_callback_is_valid(cb_uid)) return 0x80000100u;

    int32_t result = 0;
    if (slot == -1) {
        result = -1;
        for (int i = 0; i < 16; i++) {
            if (s_power_cb_slots[i] == 0) {
                s_power_cb_slots[i] = cb_uid;
                result = i;
                break;
            }
        }
        if (result < 0) return 0x80000022u;
    } else {
        if (s_power_cb_slots[slot] != 0) return 0x80000020u;
        s_power_cb_slots[slot] = cb_uid;
    }

    /* PSP hardware immediately notifies a newly registered callback. Re-registering
     * the same callback in another slot therefore increments its pending count. */
    (void)sr_callback_notify(cb_uid, 0x000010E4u);
    return (uint32_t)result;
}

/* sceKernelPowerLock(lockType) / sceKernelPowerUnlock(lockType) / sceKernelPowerTick(flag)
 * (TD-24 batch 4). Public behaviour reference: PSPSDK psppower.h and PPSSPP
 * Core/HLE/scePower.cpp. sceKernelPowerLock and sceKernelPowerUnlock validate
 * lockType (must be 0, else INVALID_MODE 0x80000107) and maintain a nested
 * lock count. sceKernelPowerTick records ticks without blocking. */
#define SCE_KERNEL_ERROR_INVALID_MODE 0x80000107u
static uint32_t s_power_lock_count = 0u;
static uint32_t s_power_tick_count = 0u;

uint32_t h_PowerLock(CpuState *s) {
    if (A0 != 0u) return SCE_KERNEL_ERROR_INVALID_MODE;
    s_power_lock_count++;
    return 0;
}

uint32_t h_PowerUnlock(CpuState *s) {
    if (A0 != 0u) return SCE_KERNEL_ERROR_INVALID_MODE;
    if (s_power_lock_count > 0u) s_power_lock_count--;
    return 0;
}

uint32_t h_PowerTick(CpuState *s) {
    (void)s;
    s_power_tick_count++;
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_power_lock_count(void) { return s_power_lock_count; }
uint32_t sr_hle_test_power_tick_count(void) { return s_power_tick_count; }
void sr_hle_test_power_lock_reset(void) { s_power_lock_count = 0u; s_power_tick_count = 0u; }
#endif

/* Registration of the scePower and sceKernelPower NIDs above (issue #307). Each function holds the
 * sr_hle_register() calls that sr_hle_init() used to make directly, in the same order. sr_hle_init()
 * still calls each one where those registrations stood, so the registration order and the final NID
 * table are unchanged. hle.c keeps the call sites; this file owns the NIDs. */

/* Power-clock retained state (TD-24 batch 2): the Int getters, the Float getters, the
 * CpuClockFrequency alias and the two SetClockFrequency variants share one definition, reached by
 * both sr_hle_init() branches, so the executable harness pins the production mapping through
 * dispatch. The getter NIDs below are the canonical #86 values: 0x478fe6f5 is the non-Int label and
 * 0xbd681969 the Int label, and both answer the same retained bus clock. 0xbd681969 was left
 * unregistered under #86 with no reason recorded for it; the PSP-3000 6.61 oracle 2026-10-15
 * measured it live at 111 (record PSP-HLE-POWER-001, case bus-clock-int), so it is registered
 * here. The Float getters write their value into $f0 (issue #86). PSP-3000 6.61 oracle
 * 2026-10-15, record PSP-HLE-POWER-001 (cases pll-clock-float, cpu-clock-float, cpu-clock-alias,
 * bus-clock-float). */
void sr_hle_power_register_clock(void) {
    sr_hle_register(0x34f9c463, "scePowerGetPllClockFrequencyInt", h_PowerGetPllClockFrequencyInt);
    sr_hle_register(0xfdb5bfe9, "scePowerGetCpuClockFrequencyInt", h_PowerGetCpuClockFrequencyInt);
    sr_hle_register(0xfee03a2f, "scePowerGetCpuClockFrequency", h_PowerGetCpuClockFrequencyInt);
    sr_hle_register(0x478fe6f5, "scePowerGetBusClockFrequency", h_PowerGetBusClockFrequencyInt);
    sr_hle_register(0xbd681969, "scePowerGetBusClockFrequencyInt", h_PowerGetBusClockFrequencyInt);
    sr_hle_register(0xea382a27, "scePowerGetPllClockFrequencyFloat", h_PowerGetPllClockFrequencyFloat);
    sr_hle_register(0xb1a52c83, "scePowerGetCpuClockFrequencyFloat", h_PowerGetCpuClockFrequencyFloat);
    sr_hle_register(0x9badb3eb, "scePowerGetBusClockFrequencyFloat", h_PowerGetBusClockFrequencyFloat);
    sr_hle_register(0x737486f2, "scePowerSetClockFrequency", h_PowerSetClockFrequency);
    sr_hle_register(0xebd177d6, "scePowerSetClockFrequency350", h_PowerSetClockFrequency350);
    /* Battery state query, in the shared helper so the executable harness registers it too. */
    sr_hle_register(0xd3075926, "scePowerIsLowBattery", h_PowerIsLowBattery);
}

/* TD-24 batch 4: sceKernelPowerLock, sceKernelPowerUnlock and sceKernelPowerTick. One definition
 * reached by both sr_hle_init() branches, so the executable harness pins the production mapping. */
void sr_hle_power_register_lock(void) {
    sr_hle_register(0xeadb1bd7, "sceKernelPowerLock", h_PowerLock);
    sr_hle_register(0x3aee7261, "sceKernelPowerUnlock", h_PowerUnlock);
    sr_hle_register(0x090ccb3f, "sceKernelPowerTick", h_PowerTick);
}

/* scePowerRegisterCallback. sr_hle_init() registers it in its production branch only. */
void sr_hle_power_register_callback(void) {
    sr_hle_register(0x04b7766e, "scePowerRegisterCallback", h_PowerRegisterCallback);
}

/* Battery getters. Issue #86: the previous numeric NIDs were bogus (absent from the PPSSPP-derived
 * nid_names.h); these canonical NIDs replace them so real title imports route here instead of being
 * unregistered. The Int alias 0xbd681969 is not a battery getter; sr_hle_power_register_clock
 * registers it. */
void sr_hle_power_register_battery(void) {
    sr_hle_register(0x2085d15d, "scePowerGetBatteryLifePercent", h_PowerGetBatteryLifePercent);
    sr_hle_register(0x1e490401, "scePowerIsBatteryCharging", h_PowerIsBatteryCharging);
    sr_hle_register(0x0afd0d8b, "scePowerIsBatteryExist", h_PowerIsBatteryExist);
    sr_hle_register(0x87440f5e, "scePowerIsPowerOnline", h_PowerIsPowerOnline);
}
