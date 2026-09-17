// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

#ifndef SR_HLE_POWER_H
#define SR_HLE_POWER_H

#include "recomp.h"

uint32_t h_PowerGetBatteryLifePercent(CpuState *s);
uint32_t h_PowerIsBatteryCharging(CpuState *s);
uint32_t h_PowerIsBatteryExist(CpuState *s);
uint32_t h_PowerIsPowerOnline(CpuState *s);
uint32_t h_PowerGetCpuClockFrequencyInt(CpuState *s);
uint32_t h_PowerGetBusClockFrequencyInt(CpuState *s);
uint32_t h_PowerRegisterCallback(CpuState *s);

int sr_callback_is_valid(uint32_t uid);
uint32_t sr_callback_notify(uint32_t uid, uint32_t notify_arg);

#endif
