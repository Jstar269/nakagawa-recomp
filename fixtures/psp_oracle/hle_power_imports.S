/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* User-mode scePower import block for CASE=hle-power-clock (read-only clock
   queries). Only query functions are declared: no Set function is imported,
   so the probe cannot change the PLL, CPU or bus clock.

   The library name and the 0x40010000 version word match the PSPSDK
   libpsppower.a stubs. The PLL pair is not shipped in libpsppower.a; its NIDs
   are the SHA-1 name-rule values (tools/gen_nidnames.py) and match
   src/rt/nid_names.h. A PSP that does not export them fails the load, which
   is a load-level result, not a clock measurement. */

#include "pspimport.s"

    IMPORT_START "scePower", 0x40010000
    IMPORT_FUNC  "scePower", 0x34F9C463, scePowerGetPllClockFrequencyInt
    IMPORT_FUNC  "scePower", 0xEA382A27, scePowerGetPllClockFrequencyFloat
    IMPORT_FUNC  "scePower", 0xFEE03A2F, scePowerGetCpuClockFrequency
    IMPORT_FUNC  "scePower", 0xFDB5BFE9, scePowerGetCpuClockFrequencyInt
    IMPORT_FUNC  "scePower", 0xB1A52C83, scePowerGetCpuClockFrequencyFloat
    IMPORT_FUNC  "scePower", 0x478FE6F5, scePowerGetBusClockFrequency
    IMPORT_FUNC  "scePower", 0xBD681969, scePowerGetBusClockFrequencyInt
    IMPORT_FUNC  "scePower", 0x9BADB3EB, scePowerGetBusClockFrequencyFloat
