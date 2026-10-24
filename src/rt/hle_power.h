// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

#ifndef SR_HLE_POWER_H
#define SR_HLE_POWER_H

#include "recomp.h"

uint32_t h_PowerGetBatteryLifePercent(CpuState *s);
uint32_t h_PowerIsBatteryCharging(CpuState *s);
uint32_t h_PowerIsBatteryExist(CpuState *s);
uint32_t h_PowerIsPowerOnline(CpuState *s);
uint32_t h_PowerGetBatteryChargingStatus(CpuState *s);
uint32_t h_PowerGetBatteryLifeTime(CpuState *s);
uint32_t h_PowerIsLowBattery(CpuState *s);
uint32_t h_PowerGetPllClockFrequencyInt(CpuState *s);
uint32_t h_PowerGetCpuClockFrequencyInt(CpuState *s);
uint32_t h_PowerGetBusClockFrequencyInt(CpuState *s);
uint32_t h_PowerGetPllClockFrequencyFloat(CpuState *s);
uint32_t h_PowerGetCpuClockFrequencyFloat(CpuState *s);
uint32_t h_PowerGetBusClockFrequencyFloat(CpuState *s);
uint32_t h_PowerSetClockFrequency(CpuState *s);
uint32_t h_PowerSetClockFrequency350(CpuState *s);
uint32_t h_PowerSetCpuClockFrequency(CpuState *s);
uint32_t h_PowerRegisterCallback(CpuState *s);
uint32_t h_PowerLock(CpuState *s);
uint32_t h_PowerUnlock(CpuState *s);
uint32_t h_PowerTick(CpuState *s);

int sr_callback_is_valid(uint32_t uid);
uint32_t sr_callback_notify(uint32_t uid, uint32_t notify_arg);

/* Registration of the NIDs above (issue #307). sr_hle_init() calls each one where its
 * registrations stood before the move, so the registration order is unchanged. */
void sr_hle_power_register_clock(void);
void sr_hle_power_register_lock(void);
void sr_hle_power_register_callback(void);
void sr_hle_power_register_battery(void);
void sr_hle_power_loadexec_reset(void); /* drop per-program callbacks and lock state */

#ifdef SR_HLE_THREAD_SELFTEST
uint32_t sr_hle_test_power_lock_count(void);
uint32_t sr_hle_test_power_tick_count(void);
void sr_hle_test_power_lock_reset(void);
void sr_hle_test_power_set_external(int external);
#endif

#endif
