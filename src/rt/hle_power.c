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

/* Retained clock request in MHz. Public behaviour reference: PSPSDK
 * src/power/psppower.h (scePowerSetClockFrequency) and PPSSPP
 * Core/HLE/scePower.cpp, where the Set calls update the frequencies the Get
 * calls report. The getters below previously returned fixed 333/166; they now
 * reflect the last accepted Set request. Frequency validation, the PLL
 * parameter, and any 350 MHz ceiling difference between the two Set variants
 * are UNMEASURED here: every request is retained verbatim and reported back. */
static uint32_t s_cpu_freq = 333u, s_bus_freq = 166u;

uint32_t h_PowerGetCpuClockFrequencyInt(CpuState *s) { (void)s; return s_cpu_freq; }
uint32_t h_PowerGetBusClockFrequencyInt(CpuState *s) { (void)s; return s_bus_freq; }

/* scePowerSetClockFrequency(pllfreq, cpufreq, busfreq): retain the requested
 * CPU/bus clocks. Same public references as the retained state above. */
uint32_t h_PowerSetClockFrequency(CpuState *s) {
    (void)A0;
    s_cpu_freq = A1;
    s_bus_freq = A2;
    return 0;
}

/* scePowerSetClockFrequency350(pllfreq, cpufreq, busfreq): same shape, same
 * retained state. Whether firmware 350 permits a distinct PLL ceiling is
 * UNMEASURED here. */
uint32_t h_PowerSetClockFrequency350(CpuState *s) {
    (void)A0;
    s_cpu_freq = A1;
    s_bus_freq = A2;
    return 0;
}

#ifdef SR_HLE_THREAD_SELFTEST
/* Test-build-only reset so the executable harness can isolate the retained
 * clock fixtures from one another. Adds no production behaviour. */
void sr_hle_test_power_reset(void) { s_cpu_freq = 333u; s_bus_freq = 166u; }
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
