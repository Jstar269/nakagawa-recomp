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
   InitStart (as nkUtil* stubs, because PSPSDK headers declare some of them returning void
   while the calls return a status), each only under its own CASE_ID, which the Makefile passes to the assembler as -DPSP_ORACLE_CASE.
   Case 161 (hle-utility-dialogs) declares the InitStart, GetStatus, Update and ShutdownStart
   calls of savedata, netconf, game sharing and the gamedata install dialog, the message dialog's
   InitStart alone (the case only ever passes it a size-0 block, which no dialog accepts), and
   the net module load and unload pair; the dialog calls are nkUtil* stubs for the reason above.
   The Makefile pins each build to exactly its sceUtility set (--exact-library). Each NID
   is the first four bytes, little-endian, of SHA-1 of the export name, and each matches
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
    IMPORT_FUNC  "sceUtility", 0x8874DBE0, nkUtilSavedataGetStatus
    IMPORT_FUNC  "sceUtility", 0xD4B95FFB, nkUtilSavedataUpdate
    IMPORT_FUNC  "sceUtility", 0x9790B33C, nkUtilSavedataShutdownStart
    IMPORT_FUNC  "sceUtility", 0x9A1C91D7, nkUtilMsgDialogGetStatus
    IMPORT_FUNC  "sceUtility", 0x95FC253B, nkUtilMsgDialogUpdate
    IMPORT_FUNC  "sceUtility", 0x67AF3428, nkUtilMsgDialogShutdownStart
    IMPORT_FUNC  "sceUtility", 0x6332AA39, nkUtilNetconfGetStatus
    IMPORT_FUNC  "sceUtility", 0x91E70E35, nkUtilNetconfUpdate
    IMPORT_FUNC  "sceUtility", 0xF88155F6, nkUtilNetconfShutdownStart
    IMPORT_FUNC  "sceUtility", 0xB57E95D9, nkUtilGamedataInstallGetStatus
    IMPORT_FUNC  "sceUtility", 0x4AECD179, nkUtilGamedataInstallUpdate
    IMPORT_FUNC  "sceUtility", 0x32E32DCB, nkUtilGamedataInstallShutdownStart
    IMPORT_FUNC  "sceUtility", 0x946963F3, nkUtilGameSharingGetStatus
    IMPORT_FUNC  "sceUtility", 0x7853182D, nkUtilGameSharingUpdate
    IMPORT_FUNC  "sceUtility", 0xEFC6F80F, nkUtilGameSharingShutdownStart
#endif
#if PSP_ORACLE_CASE == 161
    IMPORT_FUNC  "sceUtility", 0x50C4CD57, nkUtilSavedataInitStart
    IMPORT_FUNC  "sceUtility", 0x8874DBE0, nkUtilSavedataGetStatus
    IMPORT_FUNC  "sceUtility", 0xD4B95FFB, nkUtilSavedataUpdate
    IMPORT_FUNC  "sceUtility", 0x9790B33C, nkUtilSavedataShutdownStart
    IMPORT_FUNC  "sceUtility", 0x2AD8E239, nkUtilMsgDialogInitStart
    IMPORT_FUNC  "sceUtility", 0x4DB1E739, nkUtilNetconfInitStart
    IMPORT_FUNC  "sceUtility", 0x6332AA39, nkUtilNetconfGetStatus
    IMPORT_FUNC  "sceUtility", 0x91E70E35, nkUtilNetconfUpdate
    IMPORT_FUNC  "sceUtility", 0xF88155F6, nkUtilNetconfShutdownStart
    IMPORT_FUNC  "sceUtility", 0x24AC31EB, nkUtilGamedataInstallInitStart
    IMPORT_FUNC  "sceUtility", 0xB57E95D9, nkUtilGamedataInstallGetStatus
    IMPORT_FUNC  "sceUtility", 0x4AECD179, nkUtilGamedataInstallUpdate
    IMPORT_FUNC  "sceUtility", 0x32E32DCB, nkUtilGamedataInstallShutdownStart
    IMPORT_FUNC  "sceUtility", 0xC492F751, nkUtilGameSharingInitStart
    IMPORT_FUNC  "sceUtility", 0x946963F3, nkUtilGameSharingGetStatus
    IMPORT_FUNC  "sceUtility", 0x7853182D, nkUtilGameSharingUpdate
    IMPORT_FUNC  "sceUtility", 0xEFC6F80F, nkUtilGameSharingShutdownStart
    IMPORT_FUNC  "sceUtility", 0x1579A159, sceUtilityLoadNetModule
    IMPORT_FUNC  "sceUtility", 0x64D50C56, sceUtilityUnloadNetModule
#endif
