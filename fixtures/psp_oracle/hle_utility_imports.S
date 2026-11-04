/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* User-mode sceUtility import block for CASE=hle-sysparam and CASE=hle-savedata-getsize.
   Only the two system-parameter Get functions are declared for every case; no
   SetSystemParam function is imported, so the probe cannot write a setting. Library name
   and version word match the PSPSDK libpsputility.a stubs.

   The rules from threadman_user_imports.S apply: IMPORT_START emits a global
   __stub_module_sceUtility, so an SDK sceUtility stub that is still pulled lands in a
   second, non-adjacent stub run that psp-fixup-imports refuses. The PSPSDK CRT pulls one
   in every PRX: libcglue's __timezone_update, run by __libcglue_init, calls
   sceUtilityGetSystemParamInt. That is why the Get pair is declared for every case that
   links this file, not only for hle-sysparam.

   Case 104 (hle-savedata-getsize) also declares the four savedata dialog calls, and case 140
   (hle-utility-none) the GetStatus, Update and ShutdownStart calls of five dialogs and no
   InitStart, each only under its own CASE_ID, which the Makefile passes to the assembler as -DPSP_ORACLE_CASE.
   The Makefile pins each build to exactly its sceUtility set (--exact-library). Each NID
   is the first four bytes, little-endian, of SHA-1 of the export name, and all six match
   src/rt/nid_names.h. */

#include "pspimport.s"

    IMPORT_START "sceUtility", 0x40010000
    IMPORT_FUNC  "sceUtility", 0xA5DA2406, sceUtilityGetSystemParamInt
    IMPORT_FUNC  "sceUtility", 0x34B78343, sceUtilityGetSystemParamString
#if PSP_ORACLE_CASE == 104
    IMPORT_FUNC  "sceUtility", 0x50C4CD57, sceUtilitySavedataInitStart
    IMPORT_FUNC  "sceUtility", 0x8874DBE0, sceUtilitySavedataGetStatus
    IMPORT_FUNC  "sceUtility", 0xD4B95FFB, sceUtilitySavedataUpdate
    IMPORT_FUNC  "sceUtility", 0x9790B33C, sceUtilitySavedataShutdownStart
#endif
#if PSP_ORACLE_CASE == 140
    IMPORT_FUNC  "sceUtility", 0x8874DBE0, sceUtilitySavedataGetStatus
    IMPORT_FUNC  "sceUtility", 0xD4B95FFB, sceUtilitySavedataUpdate
    IMPORT_FUNC  "sceUtility", 0x9790B33C, sceUtilitySavedataShutdownStart
    IMPORT_FUNC  "sceUtility", 0x9A1C91D7, sceUtilityMsgDialogGetStatus
    IMPORT_FUNC  "sceUtility", 0x95FC253B, sceUtilityMsgDialogUpdate
    IMPORT_FUNC  "sceUtility", 0x67AF3428, sceUtilityMsgDialogShutdownStart
    IMPORT_FUNC  "sceUtility", 0x6332AA39, sceUtilityNetconfGetStatus
    IMPORT_FUNC  "sceUtility", 0x91E70E35, sceUtilityNetconfUpdate
    IMPORT_FUNC  "sceUtility", 0xF88155F6, sceUtilityNetconfShutdownStart
    IMPORT_FUNC  "sceUtility", 0xB57E95D9, sceUtilityGamedataInstallGetStatus
    IMPORT_FUNC  "sceUtility", 0x4AECD179, sceUtilityGamedataInstallUpdate
    IMPORT_FUNC  "sceUtility", 0x32E32DCB, sceUtilityGamedataInstallShutdownStart
    IMPORT_FUNC  "sceUtility", 0x946963F3, sceUtilityGameSharingGetStatus
    IMPORT_FUNC  "sceUtility", 0x7853182D, sceUtilityGameSharingUpdate
    IMPORT_FUNC  "sceUtility", 0xEFC6F80F, sceUtilityGameSharingShutdownStart
#endif
