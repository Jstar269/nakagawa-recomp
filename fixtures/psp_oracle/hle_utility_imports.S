/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* User-mode sceUtility import block for CASE=hle-sysparam. Only the two
   system-parameter Get functions are declared; no SetSystemParam function is
   imported, so the probe cannot write a setting. Library name and version word
   match the PSPSDK libpsputility.a stubs. */

#include "pspimport.s"

    IMPORT_START "sceUtility", 0x40010000
    IMPORT_FUNC  "sceUtility", 0xA5DA2406, sceUtilityGetSystemParamInt
    IMPORT_FUNC  "sceUtility", 0x34B78343, sceUtilityGetSystemParamString
