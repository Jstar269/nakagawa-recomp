// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

#include "hle_power.h"

#define A0 (s->r[4])
#define A1 (s->r[5])

/* scePower */
uint32_t h_PowerGetBatteryLifePercent(CpuState *s) { (void)s; return 100; }
uint32_t h_PowerIsBatteryCharging(CpuState *s) { (void)s; return 1; }
uint32_t h_PowerIsBatteryExist(CpuState *s) { (void)s; return 1; }
uint32_t h_PowerIsPowerOnline(CpuState *s) { (void)s; return 1; }
uint32_t h_PowerGetCpuClockFrequencyInt(CpuState *s) { (void)s; return 333; }
uint32_t h_PowerGetBusClockFrequencyInt(CpuState *s) { (void)s; return 166; }

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
